#include "design.h"
#include "workflow_sessions.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using kit::JsonValue;
using Json = nlohmann::json;

constexpr auto type = DesignArtifactType;
constexpr std::string_view prefix = "preview-design-";
constexpr std::string_view design_resource = "design-board/";
constexpr std::array<std::string_view, 10> design_kinds{
    "heading", "body", "caption", "card", "field", "button", "secondary_button", "link", "toggle", "checkbox"};

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

JsonValue Value(const Json& json) {
  if (json.is_object()) {
    JsonValue::Object fields;
    for (auto it = json.begin(); it != json.end(); ++it) fields.emplace(it.key(), Value(it.value()));
    return {std::move(fields)};
  }
  if (json.is_array()) {
    JsonValue::Array items;
    for (const auto& item : json) items.push_back(Value(item));
    return {std::move(items)};
  }
  if (json.is_string()) return {json.get<std::string>()};
  if (json.is_boolean()) return {json.get<bool>()};
  if (json.is_number_integer()) return {json.get<std::int64_t>()};
  if (json.is_number_float()) return {json.get<double>()};
  return {};
}

Json Document(const JsonValue& value) {
  return std::visit([](const auto& item) -> Json {
    using Item = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<Item, JsonValue::Object>) {
      auto object = Json::object();
      for (const auto& [key, entry] : item) object[key] = Document(entry);
      return object;
    } else if constexpr (std::is_same_v<Item, JsonValue::Array>) {
      auto array = Json::array();
      for (const auto& entry : item) array.push_back(Document(entry));
      return array;
    } else {
      return item;
    }
  }, value.value);
}

bool SafeId(std::string_view id) {
  return !id.empty() && id.size() <= 80 && std::ranges::all_of(id, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
  });
}

std::optional<huxerui::File> DesignFile(const huxerui::File& directory, std::string_view handle) {
  if (!SafeId(handle)) return {};
  return directory.Child(std::string(handle) + ".design.json");
}

const JsonValue* Property(const JsonValue& object, std::string_view key) {
  const auto* items = std::get_if<JsonValue::Object>(&object.value);
  if (!items) return nullptr;
  auto found = items->find(key);
  return found == items->end() ? nullptr : &found->second;
}

std::optional<kit::ArtifactRef> Base(const JsonValue& value, std::string_view key = "base") {
  const auto* base = Property(value, key);
  if (!base || std::holds_alternative<std::nullptr_t>(base->value)) return {};
  const auto* name = Property(*base, "id");
  const auto* revision = Property(*base, "version");
  if (!name || !revision) throw std::invalid_argument("Design revision has no base artifact");
  const auto* id = std::get_if<std::string>(&name->value);
  const auto* version = std::get_if<std::int64_t>(&revision->value);
  if (!id || !id->starts_with(prefix) || !SafeId(*id) || !version || *version < 1) {
    throw std::invalid_argument("Design revision has an invalid base artifact");
  }
  return kit::ArtifactRef{{*id}, static_cast<kit::Revision>(*version)};
}

kit::ResourceRef BoardResource(const kit::ArtifactRef& artifact) {
  return {{std::string(design_resource) + artifact.id.value}, std::to_string(artifact.version)};
}

kit::ResourceEffect BoardPublicationEffect(const kit::ArtifactRef& before, const kit::ArtifactRef& after) {
  const auto prior = BoardResource(before);
  return {prior.id, kit::ResourceEffectState::Applied, prior, BoardResource(after),
      kit::UndoRef{prior.version, kit::UndoSupport::RestoreVersion}};
}

std::optional<kit::ArtifactRef> BoardVersion(const std::optional<kit::ResourceRef>& resource) {
  if (!resource || !resource->id.value.starts_with(design_resource)) return {};
  const auto id = resource->id.value.substr(design_resource.size());
  if (!id.starts_with(prefix) || !SafeId(id)) return {};
  kit::Revision version = 0;
  const auto& text = resource->version;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), version);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !version) return {};
  return kit::ArtifactRef{{id}, version};
}

JsonValue DesignReference(const kit::ArtifactRef& ref) {
  return Value(Json{{"id", ref.id.value}, {"version", ref.version}});
}

kit::Async<kit::ArtifactDraft> StageDesignDocument(huxerui::File directory, kit::ToolContext context, const JsonValue& document,
    std::optional<kit::ArtifactRef> base, std::optional<kit::ArtifactRef> source) {
  using Result = kit::AgentResult<kit::ArtifactDraft>;
  auto verified = ParseDesign(document);
  if (!verified.Succeeded()) co_return Result::Failure(verified.Error());
  if (base && base->version == UINT64_MAX) co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::Conflict, "The design version cannot advance");
  const auto handle = context.CallId().value;
  auto file = DesignFile(directory, handle);
  if (!file || !co_await directory.CreateDirectoriesAsync()) {
    co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::ProviderFailure, "Could not open the design staging directory");
  }
  const auto bytes = Document(document).dump();
  if (context.StopToken().stop_requested()) {
    co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::AlreadyClosed, "Design publication was cancelled");
  }
  if (!co_await file->WriteStringAsync(bytes)) {
    co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::ProviderFailure, "Could not save the design document");
  }
  kit::ArtifactDraft draft{.id = base ? base->id : kit::ArtifactId{std::string(prefix) + handle},
      .expected_latest = base ? std::optional{base->version} : std::nullopt,
      .type = std::string(type), .name = verified.Value().title, .storage_handle = handle, .size_bytes = bytes.size()};
  if (source) draft.dependencies.push_back({.input = {*source}});
  for (const auto& node : verified.Value().nodes) draft.parts.push_back({{node.id}, node.label});
  if (base) draft.published_effect = BoardPublicationEffect(*base, {base->id, base->version + 1});
  co_return Result::Success(std::move(draft));
}

kit::Async<kit::ArtifactPreview> ReadDesignVersion(const std::shared_ptr<kit::AgentRuntime>& runtime, kit::SessionId session,
                                                   kit::ArtifactRef artifact) {
  using Result = kit::AgentResult<kit::ArtifactPreview>;
  auto record = co_await runtime->Artifacts().ReadAsync(artifact);
  if (!record.Succeeded()) co_return Result::Failure(record.Error());
  if (record.Value().session != session || record.Value().metadata.type != type)
    co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::PermissionDenied, "The design version belongs to another conversation");
  auto preview = co_await runtime->Artifacts().PreviewAsync(artifact);
  if (preview.Succeeded() && !preview.Value().structured)
    co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "The design version has no document");
  co_return preview;
}

kit::CommandId Stage(const kit::RunContext& run, std::string_view stage) {
  return {"design:" + run.Id().value + "/" + std::string(stage)};
}

kit::Async<kit::ToolResult> Invoke(kit::RunContext run, std::string_view stage, kit::ToolRequest request) {
  auto receipt = co_await run.StartToolAsync(Stage(run, stage), std::move(request));
  if (!receipt.Succeeded()) co_return kit::AgentResult<kit::ToolResult>::Failure(receipt.Error());
  auto result = co_await run.WaitForToolAsync(std::get<kit::ToolCallAccepted>(receipt.Value().outcome).call);
  if (result.Succeeded() && result.Value().outcome != kit::ToolOutcome::Succeeded) {
    co_return kit::AgentResult<kit::ToolResult>::Failure(result.Value().reason.value_or(kit::AgentError{
        .code = kit::ErrorCode::InvalidState, .message = "Design tool did not succeed"}));
  }
  co_return result;
}

kit::AgentResult<DesignBoard> VerifyGeneratedDesign(const std::optional<JsonValue>& document) {
  if (!document) return Fail<DesignBoard>(kit::ErrorCode::ProviderFailure, "The model returned no design document");
  auto parsed = ParseDesign(*document);
  if (!parsed.Succeeded()) return parsed;
  const auto& nodes = parsed.Value().nodes;
  const auto contains = [](const DesignNode& card, const DesignNode& node) {
    return card.kind == "card" && node.x >= card.x && node.y >= card.y &&
        node.x + node.width <= card.x + card.width && node.y + node.height <= card.y + card.height;
  };
  for (auto first = nodes.begin(); first != nodes.end(); ++first) {
    for (auto second = std::next(first); second != nodes.end(); ++second) {
      const bool intersect = first->x < second->x + second->width && second->x < first->x + first->width &&
          first->y < second->y + second->height && second->y < first->y + first->height;
      if (intersect && !contains(*first, *second) && !contains(*second, *first))
        return Fail<DesignBoard>(kit::ErrorCode::InvalidArgument, "Overlapping elements: " + first->id + " and " + second->id);
    }
  }
  return parsed;
}

} // namespace

kit::JsonSchema DesignSchema() {
  static const auto schema = [] {
    auto document = Json::parse(R"({
    "type":"object",
    "additionalProperties":false,
    "required":["title","brief","width","height","nodes"],
    "properties":{
      "title":{"type":"string"},
      "brief":{"type":"string"},
      "width":{"type":"integer"},
      "height":{"type":"integer"},
      "nodes":{
        "type":"array",
        "items":{
          "type":"object","additionalProperties":false,
          "required":["id","kind","label","note","x","y","width","height"],
          "properties":{
            "id":{"type":"string"},
            "kind":{"type":"string"},
            "label":{"type":"string"},
            "note":{"type":"string"},
            "x":{"type":"integer"},
            "y":{"type":"integer"},
            "width":{"type":"integer"},
            "height":{"type":"integer"}
          }
        }
      }
    }
  })");
    document["properties"]["nodes"]["items"]["properties"]["kind"]["enum"] = design_kinds;
    return Value(document);
  }();
  return {schema};
}

kit::AgentResult<DesignBoard> ParseDesign(const JsonValue& value) {
  try {
    const auto doc = Document(value);
    auto text = [](const Json& entry, const char* name, std::size_t limit, bool optional = false) {
      auto value = entry.at(name).get<std::string>();
      if (value.size() > limit || (!optional && value.find_first_not_of(" \t\r\n") == std::string::npos)) {
        throw std::invalid_argument(std::string(name) + " is empty or too long");
      }
      return value;
    };
    auto number = [](const Json& entry, const char* name, int min, int max) {
      const auto& value = entry.at(name);
      if (!value.is_number_integer() || value.get<std::int64_t>() < min || value.get<std::int64_t>() > max) {
        throw std::invalid_argument(std::string(name) + " is outside the artboard");
      }
      return value.get<int>();
    };
    DesignBoard design{text(doc, "title", 640), text(doc, "brief", 4800),
        number(doc, "width", 320, 1600), number(doc, "height", 220, 1200)};
    const auto& nodes = doc.at("nodes");
    if (!nodes.is_array() || nodes.empty() || nodes.size() > 24) throw std::invalid_argument("Expected one to 24 nodes");
    std::set<std::string> ids;
    for (const auto& entry : nodes) {
      DesignNode node{text(entry, "id", 64), text(entry, "kind", 16), text(entry, "label", 640), text(entry, "note", 1920, true),
          number(entry, "x", 0, design.width - 1), number(entry, "y", 0, design.height - 1),
          number(entry, "width", 1, design.width), number(entry, "height", 1, design.height)};
      if (!SafeId(node.id) || !ids.insert(node.id).second || std::ranges::find(design_kinds, node.kind) == design_kinds.end() ||
          node.x + node.width > design.width || node.y + node.height > design.height) {
        throw std::invalid_argument("Each node needs a unique stable ID, supported component and valid bounds");
      }
      design.nodes.push_back(std::move(node));
    }
    return kit::AgentResult<DesignBoard>::Success(std::move(design));
  } catch (const std::exception& error) {
    return Fail<DesignBoard>(kit::ErrorCode::InvalidArgument, error.what());
  }
}

JsonValue SampleDesign(std::string title) {
  if (title.empty() || title.size() > 148) title = "Settings";
  return Value(Json{{"title", std::move(title)}, {"brief", "Group preferences by purpose and make the current value visible."},
      {"width", 720}, {"height", 420}, {"nodes", Json::array({
          {{"id", "heading"}, {"kind", "heading"}, {"label", "Preferences"}, {"note", "Start with a clear name for the area."},
           {"x", 36}, {"y", 28}, {"width", 648}, {"height", 58}},
          {{"id", "appearance"}, {"kind", "card"}, {"label", "Appearance"}, {"note", "Related options share the same section."},
           {"x", 36}, {"y", 100}, {"width", 648}, {"height", 183}},
          {{"id", "theme"}, {"kind", "field"}, {"label", "Theme   \xc2\xb7   Use system setting"},
           {"note", "Default to the system setting and expose available choices."},
           {"x", 58}, {"y", 157}, {"width", 604}, {"height", 46}},
          {{"id", "contrast"}, {"kind", "toggle"}, {"label", "Increase contrast"},
           {"note", "The preference can be changed without leaving this screen."},
           {"x", 58}, {"y", 218}, {"width", 604}, {"height", 42}},
          {{"id", "save"}, {"kind", "button"}, {"label", "Save changes"}, {"note", "Keep the primary action near the bottom."},
           {"x", 502}, {"y", 340}, {"width", 182}, {"height", 45}},
          {{"id", "appearance-title"}, {"kind", "caption"}, {"label", "Appearance"}, {"note", ""},
           {"x", 58}, {"y", 119}, {"width", 604}, {"height", 25}}
      })}});
}

kit::ArtifactStore WithDesignArtifacts(kit::ArtifactStore fallback, huxerui::File directory) {
  auto preview = [fallback = std::move(fallback.preview), directory](
      kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
    if (!request.artifact.id.value.starts_with(prefix)) co_return co_await fallback(std::move(request));
    auto file = DesignFile(directory, request.storage_handle);
    if (!file) co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "Design version unavailable");
    auto text = co_await file->ReadStringAsync();
    if (!text.Succeeded()) co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "Design document is missing");
    try {
      auto doc = Value(Json::parse(text.Value()));
      auto design = ParseDesign(doc);
      if (!design.Succeeded()) co_return kit::AgentResult<kit::ArtifactPreview>::Failure(design.Error());
      co_return kit::AgentResult<kit::ArtifactPreview>::Success(
          {.artifact = request.artifact, .structured = std::move(doc), .summary = design.Value().title});
    } catch (const Json::exception&) {
      co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::InvalidState, "The saved design document is invalid");
    }
  };
  return {
      .preview = preview,
      .open = [fallback = fallback.open, directory](kit::ArtifactResolveRequest request) -> kit::Async<huxerui::AsyncInputStream> {
        if (!request.artifact.id.value.starts_with(prefix)) co_return co_await fallback(std::move(request));
        auto file = DesignFile(directory, request.storage_handle);
        if (!file) co_return Fail<huxerui::AsyncInputStream>(kit::ErrorCode::NotFound, "Design version unavailable");
        auto source = co_await file->OpenReadAsync();
        if (!source.Succeeded()) co_return Fail<huxerui::AsyncInputStream>(kit::ErrorCode::NotFound, "Design document is missing");
        co_return kit::AgentResult<huxerui::AsyncInputStream>::Success(std::move(source).Value());
      },
      .selection = [preview, fallback = std::move(fallback.selection)](
          kit::ArtifactSelectionRequest request) -> kit::Async<std::string> {
        if (!request.resource.artifact.id.value.starts_with(prefix)) {
          if (fallback) co_return co_await fallback(std::move(request));
          co_return Fail<std::string>(kit::ErrorCode::Unsupported, "The artifact has no text selection adapter");
        }
        auto artifact = co_await preview(std::move(request.resource));
        if (!artifact.Succeeded()) co_return kit::AgentResult<std::string>::Failure(artifact.Error());
        auto design = ParseDesign(*artifact.Value().structured);
        if (!design.Succeeded()) co_return kit::AgentResult<std::string>::Failure(design.Error());
        const auto& board = design.Value();
        std::string text;
        const auto append = [&](const DesignNode& node) {
          if (!text.empty()) text += "\n\n";
          text += node.label;
          if (!node.note.empty()) text += "\n" + node.note;
        };
        if (request.parts.empty()) {
          text = board.title + "\n" + board.brief;
          for (const auto& node : board.nodes) append(node);
        } else {
          for (const auto& part : request.parts) {
            const auto node = std::ranges::find(board.nodes, part.value, &DesignNode::id);
            if (node == board.nodes.end())
              co_return Fail<std::string>(kit::ErrorCode::NotFound, "The selected design node is unavailable");
            append(*node);
          }
        }
        if (text.size() > request.byte_limit)
          co_return Fail<std::string>(kit::ErrorCode::BudgetExceeded, "The selected design text exceeds its byte limit");
        co_return kit::AgentResult<std::string>::Success(std::move(text));
      }};
}

bool HasDesignTargets(const std::vector<kit::TargetBaseline>& targets) {
  return std::ranges::any_of(targets, [](const auto& target) { return target.target.value.starts_with(design_resource); });
}

kit::TargetBaseline DesignTargetBaseline(const kit::ArtifactRef& current) {
  const auto resource = BoardResource(current);
  return {resource.id, resource};
}

kit::ToolResourceResolver DesignChangeResources(std::shared_ptr<std::weak_ptr<kit::AgentRuntime>> runtime) {
  return [runtime](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
    using Result = kit::AgentResult<std::vector<kit::ResourceAccess>>;
    const auto owner = runtime->lock();
    if (!owner) co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::RuntimeClosed, "Design runtime unavailable");
    if (invocation.prepared_targets.size() != 1)
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::InvalidArgument, "Select one target design");
    const auto& target = invocation.prepared_targets.front();
    const auto base = BoardVersion(target.expected);
    if (!base || target.target != target.expected->id)
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::InvalidArgument, "The design baseline is unavailable");
    auto opened = co_await owner->Sessions().OpenAsync(invocation.session);
    if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
    auto snapshot = opened.Value()->Snapshot();
    if (!snapshot) co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::NotFound, "Design conversation unavailable");
    const auto current = std::ranges::find(snapshot->artifacts.rbegin(), snapshot->artifacts.rend(), base->id, &kit::ArtifactRef::id);
    if (current == snapshot->artifacts.rend())
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::NotFound, "Design unavailable in this conversation");
    auto record = co_await owner->Artifacts().ReadAsync(*current);
    if (!record.Succeeded()) co_return Result::Failure(record.Error());
    if (record.Value().session != invocation.session || record.Value().metadata.type != type)
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::PermissionDenied, "Design change target is unavailable");
    co_return Result::Success({{target.target, BoardResource(*current), invocation.phase == kit::ToolAccessPhase::Prepare
        ? kit::ResourceAccessMode::Read : kit::ResourceAccessMode::Write, {}}});
  };
}

kit::ChangeAdapters WithDesignChanges(kit::ChangeAdapters changes, huxerui::File directory,
                                     std::shared_ptr<std::weak_ptr<kit::AgentRuntime>> runtime) {
  changes.prepare_revert = [fallback = std::move(changes.prepare_revert), runtime](
      kit::ChangePrepareContext context, kit::RevertRequest request) -> kit::Async<kit::PreparedChange> {
    using Result = kit::AgentResult<kit::PreparedChange>;
    if (!HasDesignTargets(request.expected_current)) {
      if (fallback) co_return co_await fallback(std::move(context), std::move(request));
      co_return Fail<kit::PreparedChange>(kit::ErrorCode::Unsupported, "No restore adapter is configured for the selected targets");
    }
    const auto owner = runtime->lock();
    if (!owner) co_return Fail<kit::PreparedChange>(kit::ErrorCode::RuntimeClosed, "Design runtime unavailable");
    if (context.original_effects.size() != 1)
      co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "Select one design to restore");
    const auto& effect = context.original_effects.front();
    const auto current = BoardVersion(effect.after);
    const auto previous = BoardVersion(effect.before);
    if (!current || !previous || current->id != previous->id || previous->version >= current->version ||
        !effect.undo || effect.undo->support != kit::UndoSupport::RestoreVersion || effect.undo->handle != effect.before->version) {
      co_return Fail<kit::PreparedChange>(kit::ErrorCode::Unsupported, "The original design restore record is unavailable");
    }
    auto from = co_await ReadDesignVersion(owner, request.session, *current);
    if (!from.Succeeded()) co_return Result::Failure(from.Error());
    auto to = co_await ReadDesignVersion(owner, request.session, *previous);
    if (!to.Succeeded()) co_return Result::Failure(to.Error());
    co_return Result::Success({.preview = {.title = "Review design restore",
        .summary = "Current v" + std::to_string(current->version) + "   \xc2\xb7   " + from.Value().summary +
            "\nRestore v" + std::to_string(previous->version) + "   \xc2\xb7   " + to.Value().summary,
        .artifacts = {*current, *previous}},
        .targets = std::move(request.expected_current), .inputs = {{*current}, {*previous}},
        .execution_data = JsonValue{JsonValue::Object{{"base", DesignReference(*current)}, {"restore", DesignReference(*previous)}}},
        .undo_support = kit::UndoSupport::RestoreVersion});
  };
  changes.execute_revert = [fallback = std::move(changes.execute_revert), runtime, directory](
      kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
    using Result = kit::AgentResult<kit::ToolExecutionResult>;
    const auto reject = [](kit::AgentError reason) { return Result::Success({.outcome = kit::ToolOutcome::Failed, .reason = std::move(reason)}); };
    if (!HasDesignTargets(call.change.targets)) {
      if (fallback) co_return co_await fallback(std::move(context), std::move(call));
      co_return reject({.code = kit::ErrorCode::Unsupported, .message = "Restore adapter unavailable"});
    }
    const auto owner = runtime->lock();
    if (!owner) co_return reject({.code = kit::ErrorCode::RuntimeClosed, .message = "Design runtime unavailable"});
    const auto current = Base(call.change.execution_data);
    const auto previous = Base(call.change.execution_data, "restore");
    if (!current || !previous || current->id != previous->id || call.change.targets.size() != 1 ||
        call.change.targets.front().expected != BoardResource(*current)) {
      co_return reject({.code = kit::ErrorCode::Conflict, .message = "The restore no longer matches the reviewed design"});
    }
    const auto record = co_await owner->Artifacts().ReadAsync(*current);
    if (!record.Succeeded()) co_return reject(record.Error());
    auto source = co_await ReadDesignVersion(owner, record.Value().session, *previous);
    if (!source.Succeeded()) co_return reject(source.Error());
    auto draft = co_await StageDesignDocument(directory, std::move(context), *source.Value().structured, current, previous);
    if (!draft.Succeeded()) co_return reject(draft.Error());
    std::vector<kit::ArtifactDraft> drafts;
    drafts.push_back(std::move(draft).Value());
    co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(drafts)});
  };
  changes.prepare_adoption = [fallback = std::move(changes.prepare_adoption), runtime](
      kit::ChangePrepareContext context, kit::AdoptCandidateRequest request) -> kit::Async<kit::PreparedChange> {
    using Result = kit::AgentResult<kit::PreparedChange>;
    if (!HasDesignTargets(request.targets) && fallback)
      co_return co_await fallback(std::move(context), std::move(request));
    if (!context.artifact || context.artifact->metadata.type != type || request.targets.size() != 1)
      co_return Fail<kit::PreparedChange>(kit::ErrorCode::Unsupported, "Adoption requires a design candidate and one target board");
    if (!request.parts.empty()) {
      kit::AgentError error{.code = kit::ErrorCode::ScopeExpansionRequired, .message = "This design adoption replaces the whole board",
          .proposed_scope = kit::ScopeExpansion{.whole_artifact = true, .summary = "Review and adopt the whole design board"}};
      co_return Result::Failure(std::move(error));
    }
    const auto owner = runtime->lock();
    const auto current = BoardVersion(request.targets.front().expected);
    if (!owner || !current) co_return Fail<kit::PreparedChange>(kit::ErrorCode::NotFound, "The target design version is unavailable");
    auto target = co_await ReadDesignVersion(owner, request.target_session, *current);
    if (!target.Succeeded()) co_return Result::Failure(target.Error());
    auto candidate = co_await ReadDesignVersion(owner, context.artifact->session, request.candidate_artifact);
    if (!candidate.Succeeded()) co_return Result::Failure(candidate.Error());
    const auto summary = "Current v" + std::to_string(current->version) + "   \xc2\xb7   " + target.Value().summary +
        "\nSelected candidate v" + std::to_string(request.candidate_artifact.version) + "   \xc2\xb7   " + candidate.Value().summary;
    co_return Result::Success({
        .preview = {.title = "Review design adoption", .summary = summary, .artifacts = {*current, request.candidate_artifact}},
        .targets = std::move(request.targets), .inputs = {{*current}, {request.candidate_artifact}},
        .execution_data = JsonValue{JsonValue::Object{{"base", DesignReference(*current)},
            {"candidate", DesignReference(request.candidate_artifact)}}},
        .undo_support = kit::UndoSupport::RestoreVersion});
  };
  changes.execute_adoption = [fallback = std::move(changes.execute_adoption), runtime, directory](
      kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
    using Result = kit::AgentResult<kit::ToolExecutionResult>;
    if (!HasDesignTargets(call.change.targets) && fallback)
      co_return co_await fallback(std::move(context), std::move(call));
    const auto reject = [](kit::AgentError reason) { return Result::Success({.outcome = kit::ToolOutcome::Failed, .reason = std::move(reason)}); };
    const auto owner = runtime->lock();
    const auto current = Base(call.change.execution_data);
    const auto selected = Base(call.change.execution_data, "candidate");
    if (!owner || !current || !selected || call.change.targets.size() != 1 ||
        call.change.targets.front().expected != BoardResource(*current)) {
      co_return reject({.code = kit::ErrorCode::Conflict, .message = "The reviewed design adoption no longer matches its target"});
    }
    auto record = co_await owner->Artifacts().ReadAsync(*selected);
    if (!record.Succeeded()) co_return reject(record.Error());
    auto candidate = co_await ReadDesignVersion(owner, record.Value().session, *selected);
    if (!candidate.Succeeded()) co_return reject(candidate.Error());
    auto draft = co_await StageDesignDocument(directory, std::move(context), *candidate.Value().structured, current, selected);
    if (!draft.Succeeded()) co_return reject(draft.Error());
    draft.Value().dependencies.push_back({.input = {*current}});
    std::vector<kit::ArtifactDraft> drafts;
    drafts.push_back(std::move(draft).Value());
    co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(drafts)});
  };
  return changes;
}

std::optional<kit::RevertRequest> DesignRevertRequest(const kit::SessionSnapshot& session, const kit::ArtifactRef& current) {
  if (std::ranges::any_of(session.artifacts, [&](const auto& ref) { return ref.id == current.id && ref.version > current.version; })) return {};
  const auto target = BoardResource(current);
  for (const auto& tool : session.tools) {
    if (tool.state != kit::ToolCallState::Succeeded || std::ranges::find(tool.artifacts, current) == tool.artifacts.end()) continue;
    const auto effect = std::ranges::find_if(tool.effects, [&](const auto& item) {
      return item.state == kit::ResourceEffectState::Applied && item.after == target && item.undo &&
          item.undo->support == kit::UndoSupport::RestoreVersion;
    });
    if (effect != tool.effects.end()) return kit::RevertRequest{session.summary.id, tool.id, {{target.id, target}}};
  }
  return {};
}

kit::AgentResult<void> RegisterDesignTools(const std::shared_ptr<kit::AgentRuntime>& runtime, huxerui::File directory) {
  const auto owner = std::weak_ptr<kit::AgentRuntime>(runtime);
  auto read = runtime->RegisterTool({
      .id = ReadDesign, .version = 1, .description = "Read the latest approved design of the current conversation",
      .input_schema = {Value(Json{{"type", "object"}, {"additionalProperties", false}})}, .effect = kit::ToolEffect::Read,
      .prepare = [owner](kit::ToolPrepareContext context, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        const auto runtime = owner.lock();
        if (!runtime || !context.actor.session) co_return Fail<kit::PreparedChange>(kit::ErrorCode::RuntimeClosed, "Design session unavailable");
        auto opened = co_await runtime->Sessions().OpenAsync(*context.actor.session);
        if (!opened.Succeeded()) co_return kit::AgentResult<kit::PreparedChange>::Failure(opened.Error());
        auto snapshot = opened.Value()->Snapshot();
        if (!snapshot) co_return Fail<kit::PreparedChange>(kit::ErrorCode::PermissionDenied, "The design session is unavailable");
        auto found = std::ranges::find_if(snapshot->artifacts.rbegin(), snapshot->artifacts.rend(),
            [](const auto& artifact) { return artifact.id.value.starts_with(prefix); });
        kit::PreparedChange plan{.preview = {.title = "Read approved design"}};
        if (found != snapshot->artifacts.rend()) {
          auto preview = co_await runtime->Artifacts().PreviewAsync(*found);
          if (!preview.Succeeded()) co_return kit::AgentResult<kit::PreparedChange>::Failure(preview.Error());
          if (!preview.Value().structured) co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidState, "The design version has no document");
          plan.inputs.push_back({*found});
          plan.execution_data = JsonValue{JsonValue::Object{
              {"document", *preview.Value().structured},
              {"base", Value(Json{{"id", found->id.value}, {"version", found->version}})}}};
        }
        co_return kit::AgentResult<kit::PreparedChange>::Success(std::move(plan));
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({
            .outcome = kit::ToolOutcome::Succeeded, .model_value = std::move(call.change.execution_data)});
      }});
  if (!read.Succeeded()) return read;
  return runtime->RegisterTool({
      .id = PublishDesign, .version = 1, .description = "Publish the confirmed design document in the current conversation",
      .input_schema = {Value(Json{{"type", "object"}, {"required", {"document", "base"}}, {"additionalProperties", false},
          {"properties", {{"document", Document(DesignSchema().document)}, {"base", true}}}})},
      .effect = kit::ToolEffect::StageArtifact,
      .prepare = [owner](kit::ToolPrepareContext context, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        auto verified = ParseDesign(*Property(request.arguments, "document"));
        if (!verified.Succeeded()) co_return kit::AgentResult<kit::PreparedChange>::Failure(verified.Error());
        kit::PreparedChange plan{.preview = {.title = "Publish design", .summary = verified.Value().title}};
        try {
          if (const auto base = Base(request.arguments)) {
            const auto runtime = owner.lock();
            if (!runtime) co_return Fail<kit::PreparedChange>(kit::ErrorCode::RuntimeClosed, "Design runtime unavailable");
            auto source = co_await runtime->Artifacts().ReadAsync(*base);
            if (!source.Succeeded()) co_return kit::AgentResult<kit::PreparedChange>::Failure(source.Error());
            if (source.Value().session != context.actor.session || source.Value().metadata.type != type) {
              co_return Fail<kit::PreparedChange>(kit::ErrorCode::PermissionDenied, "The design base belongs to another session");
            }
            plan.inputs.push_back({*base});
            plan.undo_support = kit::UndoSupport::RestoreVersion;
          }
        } catch (const std::invalid_argument& error) {
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, error.what());
        }
        plan.execution_data = std::move(request.arguments);
        co_return kit::AgentResult<kit::PreparedChange>::Success(std::move(plan));
      },
      .execute = [directory](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto& doc = *Property(call.change.execution_data, "document");
        const auto base = Base(call.change.execution_data);
        auto artifact = co_await StageDesignDocument(directory, std::move(context), doc, base, base);
        if (!artifact.Succeeded()) co_return kit::AgentResult<kit::ToolExecutionResult>::Failure(artifact.Error());
        std::vector<kit::ArtifactDraft> drafts;
        drafts.push_back(std::move(artifact).Value());
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({
            .outcome = kit::ToolOutcome::Succeeded, .model_value = doc, .new_artifacts = std::move(drafts)});
      },
      .recover_published_effect = [](const kit::ArtifactRecord& before, const kit::ArtifactRecord& after)
          -> std::optional<kit::ResourceEffect> {
        const auto& ref = after.metadata.ref;
        if (after.metadata.type != DesignArtifactType || !ref.id.value.starts_with(prefix) || !SafeId(ref.id.value)) return {};
        return BoardPublicationEffect(before.metadata.ref, ref);
      }});
}

kit::Async<kit::RunCompletion> RunDesign(kit::RunContext run, kit::RunInput input,
    std::optional<kit::RunCheckpoint> checkpoint) {
  using Completion = kit::AgentResult<kit::RunCompletion>;
  JsonValue prepared;
  if (checkpoint) {
    if (checkpoint->schema_version != 1 || checkpoint->stage != DesignReview ||
        !Property(checkpoint->state, "document") || !Property(checkpoint->state, "base")) {
      co_return Fail<kit::RunCompletion>(kit::ErrorCode::Unsupported, "Unknown design checkpoint");
    }
    prepared = std::move(checkpoint->state);
  } else {
    auto baseline = co_await Invoke(run, "base", {ReadDesign, JsonValue{JsonValue::Object{}}});
    if (!baseline.Succeeded()) co_return Completion::Failure(baseline.Error());
    std::string prompt = std::string(DesignBriefPrefix) + input.input.text +
        "\nChoose the composition for the task. Use an 800 by 520 pixel artboard for desktop when unspecified; "
        "choose a portrait artboard when the brief calls for mobile. The legal canvas is 320 to 1600 by 220 to 1200 pixels. "
        "Use 1 to 24 nodes. Use consistent alignment, at least 24 px of card padding, and 12 to 20 px between controls. "
        "Allow at least 44 px for a screen heading and for inputs and filled buttons. "
        "A card is a background grouping, and its label is only shown in the layer list. Use heading or caption for visible titles. "
        "Use button only for the main action, secondary_button for alternative actions, link for navigation, "
        "checkbox for a form option, toggle for a persistent setting, and body or caption for explanatory copy. "
        "For sign-in, distinguish the main submit action from password recovery and account creation links. "
        "Write concise, natural screen copy in the user's language. Put implementation notes in note. "
        "Give each node a stable ASCII id. Keep all nodes within the artboard and avoid overlaps other than full containment by a card.";
    const auto& latest = baseline.Value().model_value;
    const auto* previous = latest ? Property(*latest, "document") : nullptr;
    const auto* base = latest ? Property(*latest, "base") : nullptr;
    if (previous) prompt += "\nRevise this approved design according to the request. Keep the IDs of retained controls.\n" + Document(*previous).dump();
    const auto schema = DesignSchema();
    auto model = co_await run.CompleteAsync({.instruction = prompt, .output_schema = schema});
    if (!model.Succeeded()) co_return Completion::Failure(model.Error());
    auto verified = VerifyGeneratedDesign(model.Value().structured);
    if (!verified.Succeeded()) {
      prompt += std::string(DesignCorrectionPrefix) + verified.Error().message +
          ". Preserve the requirements and valid node IDs. Return the complete corrected document.";
      model = co_await run.CompleteAsync({.instruction = std::move(prompt), .output_schema = schema});
      if (!model.Succeeded()) co_return Completion::Failure(model.Error());
      verified = VerifyGeneratedDesign(model.Value().structured);
      if (!verified.Succeeded()) co_return Completion::Failure(verified.Error());
    }
    prepared = JsonValue{JsonValue::Object{
        {"document", *model.Value().structured}, {"base", base ? *base : JsonValue{}}}};
    auto saved = co_await run.SaveCheckpointAsync({.stage = std::string(DesignReview), .schema_version = 1, .state = prepared});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }
  auto approval = co_await run.RequestInputAsync(Stage(run, "review"), {
      .title = "Review design",
      .schema = {Value(Json{{"const", true}})},
      .presentation = JsonValue{JsonValue::Object{{"kind", JsonValue{std::string(DesignReview)}},
          {"design", *Property(prepared, "document")}, {"base", *Property(prepared, "base")}}}});
  if (!approval.Succeeded()) co_return Completion::Failure(approval.Error());
  auto decision = co_await run.WaitForInputAsync(std::get<kit::InputRequested>(approval.Value().outcome).id);
  if (!decision.Succeeded()) co_return Completion::Failure(decision.Error());
  if (decision.Value().state != kit::InteractionState::Submitted) {
    co_return Fail<kit::RunCompletion>(kit::ErrorCode::AlreadyClosed, "Design review closed");
  }
  auto saved = co_await Invoke(run, "publish", {PublishDesign, std::move(prepared)});
  if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  if (saved.Value().published_artifacts.size() != 1) {
    co_return Fail<kit::RunCompletion>(kit::ErrorCode::InvalidState, "No design artifact was published");
  }
  auto artifact = saved.Value().published_artifacts.front();
  if (input.task) {
    auto finished = co_await FinishProjectTask(run, *input.task, {artifact});
    if (!finished.Succeeded()) co_return Completion::Failure(finished.Error());
  }
  if (input.work_request) {
    auto reply = co_await run.Collaboration().ReplyAsync(Stage(run, "reply"), *input.work_request,
        {.status = kit::ReplyStatus::Succeeded, .summary = "The design was approved", .artifacts = {artifact}});
    if (!reply.Succeeded()) co_return Completion::Failure(reply.Error());
    if (!std::holds_alternative<kit::MessageAccepted>(reply.Value().outcome)) {
      co_return Fail<kit::RunCompletion>(kit::ErrorCode::InvalidState, "Design reply is waiting for confirmation");
    }
  }
  co_return Completion::Success({.summary = "Design approved", .artifacts = {artifact}});
}

} // namespace agentkit_preview
