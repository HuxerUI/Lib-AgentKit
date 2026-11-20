#include "runtime_internal.h"

#include <charconv>

namespace huxerui::agentkit::detail {
namespace {

using Resources = std::vector<SkillResource>;
constexpr std::size_t MaxActiveSkills = 64;

bool BoundedText(std::string_view text, std::size_t limit) {
  return !text.empty() && text.size() <= limit && ValidUtf8(text) && text.find('\0') == std::string_view::npos;
}

bool SkillIdentifier(std::string_view text) {
  return BoundedText(text, 128) &&
      std::ranges::none_of(text, [](unsigned char byte) { return byte < 32 || byte == 127; });
}

bool ResourcePath(std::string_view path) {
  if (!BoundedText(path, 1024) || path.ends_with('/') || path.find_first_of("\\:") != std::string_view::npos ||
      std::ranges::any_of(path, [](unsigned char byte) { return byte < 32 || byte == 127; })) return false;
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto part = path.substr(0, slash);
    if (part.empty() || part.ends_with('.') || part.ends_with(' ')) return false;
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return true;
}

bool SameEntry(const SkillRef& first, const SkillRef& second) {
  return first.source == second.source && first.id == second.id;
}

bool RunOpen(const RuntimeState& state, const RunExecution& run) {
  return state.stage == RuntimeState::Stage::Started &&
      !Terminal(run.view.state) && !run.stop.stop_requested();
}

AgentResult<SkillRef> SkillArguments(const JsonValue& arguments) {
  const auto* object = std::get_if<JsonValue::Object>(&arguments.value);
  if (!object) return Fail<SkillRef>(ErrorCode::InvalidArgument, "Skill arguments must be an object");
  const auto string = [&](std::string_view field) -> const std::string* {
    const auto entry = object->find(field);
    return entry != object->end() ? std::get_if<std::string>(&entry->second.value) : nullptr;
  };
  const auto* source = string("source");
  const auto* name = string("id");
  const auto* text = string("version");
  if (!source || !name || !text) return Fail<SkillRef>(ErrorCode::InvalidArgument, "Provide the source, ID and version from the Skill catalog");
  SkillRef ref{{*source}, {*name}, 0};
  const auto converted = std::from_chars(text->data(), text->data() + text->size(), ref.version);
  if (converted.ec != std::errc{} || converted.ptr != text->data() + text->size() || !ValidSkillRef(ref))
    return Fail<SkillRef>(ErrorCode::InvalidArgument, "Invalid Skill reference");
  return AgentResult<SkillRef>::Success(std::move(ref));
}

PolicyRequest SkillPolicy(const SessionData& session, const RunExecution& run, SkillActivation activation, Capability cap) {
  auto request = RuntimeState::Request(run.principal, cap, &session, &run);
  request.skill = std::move(activation.skill);
  if (activation.trigger == SkillActivationTrigger::User) request.actor.kind = ActionParty::Kind::User;
  if (activation.trigger == SkillActivationTrigger::Host) request.actor.kind = ActionParty::Kind::Host;
  return request;
}

SkillSourceRequest SourceRequest(const PolicyRequest& request, std::stop_token stop) {
  return {request.actor, *request.agent, request.project, request.workspace, stop};
}

CommandId SkillCommand(const RunId& run, const SkillActivation& activation) {
  const auto& ref = activation.skill;
  return {"agentkit.skill/" + run.value + "/" + std::to_string(ref.source.value.size()) + "/" + ref.source.value +
      "/" + ref.id.value + "/" + std::to_string(ref.version) + "/" + std::to_string(static_cast<int>(activation.trigger))};
}

bool Confirmed(const RuntimeState& state, const RunExecution& run, const SkillActivation& activation) {
  const auto found = state.commands.find({run.principal, SkillCommand(run.view.id, activation)});
  return found != state.commands.end() && found->second.input.skill == activation &&
      found->second.progress.state == CommandState::Applied;
}

} // namespace

bool ValidSkillRef(const SkillRef& ref) {
  return ref.version && SkillIdentifier(ref.source.value) && SkillIdentifier(ref.id.value);
}

bool SkillTool(std::string_view name) { return name == ActivateSkillTool || name == ReadSkillTool; }

std::optional<AgentError> CheckSkillOffer(const JsonValue& arguments, const std::vector<SkillSummary>& catalog) {
  auto ref = SkillArguments(arguments);
  if (!ref.Succeeded()) return ref.Error();
  if (std::ranges::find(catalog, ref.Value(), &SkillSummary::ref) == catalog.end())
    return Error(ErrorCode::PermissionDenied, "That Skill version was not offered for this turn");
  return {};
}

std::vector<ModelToolSpec> SkillTools(const ModelTurnRequest& request) {
  std::vector<ModelToolSpec> result;
  if (!request.available_skills.empty()) {
    static const JsonSchema schema{ParseJson(R"({"type":"object","additionalProperties":false,
      "required":["source","id","version"],"properties":{"source":{"type":"string"},
      "id":{"type":"string"},"version":{"type":"string"}}})").Value()};
    auto name = std::string(ActivateSkillTool);
    result.push_back({{{name}, 1, {}}, name, "Activate the exact Skill reference offered in the catalog", schema});
  }
  if (!request.skill_instructions.empty()) {
    static const JsonSchema schema{ParseJson(R"({"type":"object","additionalProperties":false,
      "required":["source","id","version","path"],"properties":{"source":{"type":"string"},
      "id":{"type":"string"},"version":{"type":"string"},"path":{"type":"string"}}})").Value()};
    auto name = std::string(ReadSkillTool);
    result.push_back({{{name}, 1, {}}, name, "Read a declared text resource from an active Skill version", schema});
  }
  return result;
}

Async<std::vector<SkillSummary>> RuntimeState::DiscoverSkills(PolicyRequest request, std::stop_token stop) {
  using Result = AgentResult<std::vector<SkillSummary>>;
  const auto revision = access_revision;
  const auto& agent = agents.at(request.agent->id);
  std::vector<SkillSummary> result;
  std::set<SkillSourceId> visited;
  const auto source_request = SourceRequest(request, stop);
  for (const auto& id : agent.skill_sources) {
    if (!visited.insert(id).second) continue;
    if (stop.stop_requested()) co_return Fail<std::vector<SkillSummary>>(ErrorCode::AlreadyClosed, "Skill discovery stopped");
    try {
      auto listed = co_await skill_sources.at(id)->ListAsync(source_request);
      if (!listed.Succeeded()) {
        ReportException(std::make_exception_ptr(std::runtime_error("A Skill source catalog could not be read")));
        continue;
      }
      std::set<SkillId> entries;
      std::vector<SkillSummary> permitted;
      bool valid = true;
      for (auto& entry : listed.Value()) {
        if (entry.ref.source != id || !ValidSkillRef(entry.ref) || !BoundedText(entry.name, 128) ||
            !BoundedText(entry.description, 1024) || !entries.insert(entry.ref.id).second) {
          valid = false;
          break;
        }
        request.skill = entry.ref;
        request.capability = Capability::DiscoverSkill;
        auto discoverable = co_await Require(request);
        if (!discoverable.Succeeded()) continue;
        if (request.recipient_model) {
          request.capability = Capability::SendToModel;
          auto permitted_model = co_await Require(request);
          if (!permitted_model.Succeeded()) continue;
        }
        permitted.push_back(std::move(entry));
      }
      if (valid) std::ranges::move(permitted, std::back_inserter(result));
      else ReportException(std::make_exception_ptr(std::runtime_error("A Skill source returned an invalid catalog")));
    } catch (...) {
      ReportException(std::current_exception());
    }
  }
  if (stop.stop_requested()) co_return Fail<std::vector<SkillSummary>>(ErrorCode::AlreadyClosed, "Skill discovery stopped");
  if (stage != Stage::Started || revision != access_revision)
    co_return Fail<std::vector<SkillSummary>>(ErrorCode::Conflict, "Skill catalog access changed");
  co_return Result::Success(std::move(result));
}

Async<void> RuntimeState::CheckSkillAccess(const RunExecution& run, SkillActivation activation, bool confirmed) {
  if (!RunOpen(*this, run)) co_return Fail<void>(ErrorCode::AlreadyClosed, "Run closed");
  if (!ValidSkillRef(activation.skill)) co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid Skill reference");
  const auto& ids = agents.at(run.view.agent.id).skill_sources;
  if (!skill_sources.contains(activation.skill.source) || std::ranges::find(ids, activation.skill.source) == ids.end()) {
    co_return Fail<void>(ErrorCode::PermissionDenied, "Skill source is unavailable to this assistant");
  }
  auto decision = co_await Decide(SkillPolicy(sessions.at(run.view.session), run, activation, Capability::ActivateSkill));
  if (!decision.Succeeded()) co_return AgentResult<void>::Failure(decision.Error());
  if (decision.Value().decision != AccessDecision::Allow &&
      !(confirmed && decision.Value().decision == AccessDecision::RequireConfirmation)) {
    co_return Fail<void>(ErrorCode::PermissionDenied, decision.Value().reason.empty() ?
        "This Skill activation is not authorized" : decision.Value().reason);
  }
  co_return AgentResult<void>::Success();
}

Async<SkillPackage> RuntimeState::ReadSkillPackage(const RunExecution& run, SkillActivation activation) {
  const auto& session = sessions.at(run.view.session);
  auto request = SkillPolicy(session, run, activation, Capability::ActivateSkill);
  const auto ref = activation.skill;
  const auto revision = access_revision;
  auto loaded = AgentResult<SkillPackage>::Failure(Error(ErrorCode::ProviderFailure, "Skill package could not be loaded"));
  try {
    loaded = co_await skill_sources.at(ref.source)->LoadAsync(SourceRequest(request, run.stop.get_token()), ref);
  } catch (...) {
    ReportException(std::current_exception());
  }
  if (!RunOpen(*this, run)) co_return Fail<SkillPackage>(ErrorCode::AlreadyClosed, "Run closed");
  if (revision != access_revision) co_return Fail<SkillPackage>(ErrorCode::Conflict, "Skill access changed during loading");
  if (!loaded.Succeeded()) {
    if (loaded.Error().code == ErrorCode::NotFound) co_return Fail<SkillPackage>(ErrorCode::Conflict, "The Skill version is unavailable");
    co_return loaded;
  }
  const auto& package = loaded.Value();
  if (package.ref != ref) co_return Fail<SkillPackage>(ErrorCode::Conflict, "The source returned a different Skill version");
  if (!BoundedText(package.instructions, 1 << 20) || package.resources.size() > 1024)
    co_return Fail<SkillPackage>(ErrorCode::ProtocolError, "Invalid Skill instructions or manifest");
  std::set<std::string_view> paths;
  for (const auto& resource : package.resources) {
    if (!ResourcePath(resource.relative_path) || !BoundedText(resource.media_type, 128) || !paths.insert(resource.relative_path).second)
      co_return Fail<SkillPackage>(ErrorCode::ProtocolError, "Invalid Skill resource manifest");
  }
  co_return loaded;
}

Async<Resources> RuntimeState::LoadAndActivateSkill(RunExecution& run, SkillActivation activation) {
  auto& session = sessions.at(run.view.session);
  const auto& ref = activation.skill;
  const auto current = std::ranges::find_if(run.view.active_skills, [&](const auto& item) { return SameEntry(item.skill, ref); });
  if (current != run.view.active_skills.end()) {
    const auto existing = *current;
    if (existing.skill != ref) co_return Fail<Resources>(ErrorCode::Conflict, "A different version of this Skill is already active");
    auto access = co_await CheckSkillAccess(run, activation, Confirmed(*this, run, activation));
    if (!access.Succeeded()) co_return AgentResult<Resources>::Failure(access.Error());
    auto loaded = co_await ReadSkillPackage(run, existing);
    if (!loaded.Succeeded()) co_return AgentResult<Resources>::Failure(loaded.Error());
    co_return AgentResult<Resources>::Success(std::move(loaded).Value().resources);
  }
  if (run.view.active_skills.size() >= MaxActiveSkills) co_return Fail<Resources>(ErrorCode::BudgetExceeded, "Too many active Skills");
  const auto& ids = agents.at(run.view.agent.id).skill_sources;
  if (std::ranges::find(ids, ref.source) == ids.end()) co_return Fail<Resources>(ErrorCode::PermissionDenied, "Skill source is unavailable");

  const auto command = SkillCommand(run.view.id, activation);
  CommandInput key{.operation = Operation::ActivateSkill, .session = session.summary.id, .run = run.view.id, .skill = activation};
  auto prior = Find(run.principal, command, key);
  if (!prior.Succeeded()) co_return AgentResult<Resources>::Failure(prior.Error());
  if (!prior.Value()) {
    const auto revision = access_revision;
    std::vector<Capability> confirmation;
    auto access = co_await CheckOperation(SkillPolicy(session, run, activation, Capability::ActivateSkill), confirmation, {});
    if (!access.Succeeded()) co_return AgentResult<Resources>::Failure(access.Error());
    if (revision != access_revision) co_return Fail<Resources>(ErrorCode::Conflict, "Skill activation authorization changed");
    if (!confirmation.empty()) {
      ApprovalPreview preview{.title = "Activate Skill", .summary = ref.source.value + " / " + ref.id.value +
          " (version " + std::to_string(ref.version) + ")"};
      OperationPlan plan{.confirmation = std::move(confirmation)};
      auto staged = co_await StageOperation(run.principal, command, key, session.summary.id, std::move(plan),
                                            std::move(preview), run.deadline);
      if (!staged.Succeeded()) co_return AgentResult<Resources>::Failure(staged.Error());
    }
  }

  auto saved_command = commands.find({run.principal, command});
  bool confirmed = false;
  if (saved_command != commands.end()) {
    auto& progress = saved_command->second.progress;
    if (!progress.approval) co_return Fail<Resources>(ErrorCode::InvalidState, "Skill approval binding is unavailable");
    auto answer = co_await WaitForInteraction(run.view.id, progress.approval->interaction);
    if (!answer.Succeeded()) co_return AgentResult<Resources>::Failure(answer.Error());
    if (answer.Value().state != InteractionState::Approved || progress.state != CommandState::Scheduled) {
      co_return AgentResult<Resources>::Failure(progress.failure.value_or(Error(ErrorCode::PermissionDenied, "Skill activation was not approved")));
    }
    confirmed = true;
  }

  const auto revision = access_revision;
  auto load = [this, &run, activation, revision, confirmed]() -> Async<SkillPackage> {
    auto allowed = co_await CheckSkillAccess(run, activation, confirmed);
    if (!allowed.Succeeded()) co_return AgentResult<SkillPackage>::Failure(allowed.Error());
    auto package = co_await ReadSkillPackage(run, activation);
    if (!package.Succeeded()) co_return package;
    allowed = co_await CheckSkillAccess(run, activation, confirmed);
    if (!allowed.Succeeded()) co_return AgentResult<SkillPackage>::Failure(allowed.Error());
    if (revision != access_revision || run.stop.stop_requested())
      co_return Fail<SkillPackage>(ErrorCode::Conflict, "Skill access changed before commit");
    co_return package;
  };
  auto loaded = co_await load();
  if (!RunOpen(*this, run)) co_return Fail<Resources>(ErrorCode::AlreadyClosed, "Run closed");
  ++session.sequence;
  if (loaded.Succeeded()) run.view.active_skills.push_back(std::move(activation));
  if (confirmed) {
    auto& record = saved_command->second;
    Resolve(record, session, loaded.Succeeded() ? CommandState::Applied : CommandState::Failed,
        loaded.Succeeded() ? std::optional<AgentError>{} : loaded.Error());
    record.progress.resolution->commit = {};
  }
  auto saved = co_await WhenSaved(Persist(session));
  if (!saved.Succeeded()) co_return AgentResult<Resources>::Failure(saved.Error());
  if (!loaded.Succeeded()) co_return AgentResult<Resources>::Failure(loaded.Error());
  co_return AgentResult<Resources>::Success(std::move(loaded).Value().resources);
}

Async<Resources> RuntimeState::ActivateSkill(RunId id, SkillActivation activation) {
  auto& run = runs.at(id);
  if (!RunOpen(*this, run)) co_return Fail<Resources>(ErrorCode::AlreadyClosed, "Run closed");
  if (!ValidSkillRef(activation.skill)) co_return Fail<Resources>(ErrorCode::InvalidArgument, "Invalid Skill reference");
  if (run.activating_skill) {
    if (*run.activating_skill != activation) {
      if (SameEntry(run.activating_skill->skill, activation.skill) && run.activating_skill->skill != activation.skill)
        co_return Fail<Resources>(ErrorCode::Conflict, "A different version of this Skill is being activated");
      co_return Fail<Resources>(ErrorCode::Busy, "A Skill activation is already pending");
    }
    co_return co_await AwaitCallback<AgentResult<Resources>>([&run](CallbackCompletion<AgentResult<Resources>> done) {
      run.skill_waiters.push_back(std::move(done));
      return std::function<void()>{};
    });
  }
  run.activating_skill = activation;
  auto result = Fail<Resources>(ErrorCode::ProviderFailure, "Skill activation failed");
  try { result = co_await LoadAndActivateSkill(run, std::move(activation)); }
  catch (...) { ReportException(std::current_exception()); }
  run.activating_skill.reset();
  for (const auto& waiter : std::exchange(run.skill_waiters, {})) waiter.Complete(result);
  co_return result;
}

Async<void> RuntimeState::ActivateInputSkills(RunExecution& run) {
  const auto selected = run.input.skills;
  for (const auto& ref : selected) {
    if (std::ranges::find(run.view.active_skills, ref, &SkillActivation::skill) != run.view.active_skills.end()) continue;
    SkillActivation request{ref, SkillActivationTrigger::User};
    auto used = co_await ActivateSkill(run.view.id, std::move(request));
    if (!used.Succeeded()) co_return AgentResult<void>::Failure(used.Error());
  }
  co_return AgentResult<void>::Success();
}

Async<void> RuntimeState::VerifyActiveSkills(const RunExecution& run, bool model) {
  const auto revision = access_revision;
  const auto active = run.view.active_skills;
  const auto& session = sessions.at(run.view.session);
  for (const auto& activation : active) {
    auto allowed = co_await CheckSkillAccess(run, activation, Confirmed(*this, run, activation));
    if (!allowed.Succeeded()) co_return allowed;
    if (model) {
      auto outbound = Request(run.principal, Capability::SendToModel, &session, &run);
      outbound.skill = activation.skill;
      outbound.recipient_model = run.view.model.id;
      allowed = co_await Require(std::move(outbound));
      if (!allowed.Succeeded()) co_return allowed;
    }
    auto loaded = co_await ReadSkillPackage(run, activation);
    if (!loaded.Succeeded()) co_return AgentResult<void>::Failure(loaded.Error());
  }
  if (!run.model_skill_resources.empty()) {
    auto permitted = co_await VerifyModelSkillResources(run);
    if (!permitted.Succeeded()) co_return permitted;
  }
  if (revision != access_revision)
    co_return Fail<void>(ErrorCode::Conflict, "Active Skill authorization changed");
  co_return AgentResult<void>::Success();
}

Async<void> RuntimeState::VerifyModelSkillResources(const RunExecution& run) {
  const auto& session = sessions.at(run.view.session);
  const auto resources = run.model_skill_resources;
  for (const auto& item : resources) {
    auto request = Request(run.principal, Capability::ReadSkillResource, &session, &run);
    request.skill = item.skill;
    request.skill_resource = item.path;
    auto read = co_await Require(request);
    if (!read.Succeeded()) co_return read;
    request.capability = Capability::SendToModel;
    request.recipient_model = run.view.model.id;
    auto outbound = co_await Require(std::move(request));
    if (!outbound.Succeeded()) co_return outbound;
  }
  co_return AgentResult<void>::Success();
}

Async<void> RuntimeState::PrepareSkills(const RunExecution& run, ModelTurnRequest& request) {
  const auto& session = sessions.at(run.view.session);
  auto discover = Request(run.principal, Capability::DiscoverSkill, &session, &run);
  discover.recipient_model = run.view.model.id;
  auto catalog = co_await DiscoverSkills(std::move(discover), run.stop.get_token());
  if (!catalog.Succeeded()) co_return AgentResult<void>::Failure(catalog.Error());
  request.available_skills = std::move(catalog).Value();
  const auto active = run.view.active_skills;
  for (const auto& activation : active) {
    auto allowed = co_await CheckSkillAccess(run, activation, Confirmed(*this, run, activation));
    if (!allowed.Succeeded()) co_return allowed;
    auto outbound = Request(run.principal, Capability::SendToModel, &session, &run);
    outbound.skill = activation.skill;
    outbound.recipient_model = run.view.model.id;
    allowed = co_await Require(std::move(outbound));
    if (!allowed.Succeeded()) co_return allowed;
    auto package = co_await ReadSkillPackage(run, activation);
    if (!package.Succeeded()) co_return AgentResult<void>::Failure(package.Error());
    request.skill_instructions.push_back({activation.skill, std::move(package).Value().instructions});
  }
  co_return co_await VerifyModelSkillResources(run);
}

Async<JsonValue> RuntimeState::ExecuteSkillTool(RunId id, std::string name, JsonValue arguments) {
  using Result = AgentResult<JsonValue>;
  auto ref = SkillArguments(arguments);
  if (!ref.Succeeded()) co_return Result::Failure(ref.Error());
  const auto& object = std::get<JsonValue::Object>(arguments.value);
  if (name == ActivateSkillTool && object.size() == 3) {
    SkillActivation activation{ref.Value(), SkillActivationTrigger::Model};
    auto result = co_await ActivateSkill(id, std::move(activation));
    if (!result.Succeeded()) co_return Result::Failure(result.Error());
    co_return Result::Success(JsonValue{JsonValue::Object{{"activated", JsonValue{true}}}});
  }
  const auto field = object.find("path");
  const auto* path = field != object.end() ? std::get_if<std::string>(&field->second.value) : nullptr;
  if (name != ReadSkillTool || !path || object.size() != 4)
    co_return Fail<JsonValue>(ErrorCode::InvalidArgument, "Invalid Skill tool arguments");
  auto text = co_await ReadSkillTextForModel(id, std::move(ref).Value(), *path);
  if (!text.Succeeded()) co_return Result::Failure(text.Error());
  co_return Result::Success(JsonValue{std::move(text).Value()});
}

Async<std::string> RuntimeState::ReadSkillTextForModel(RunId id, SkillRef ref, std::string path) {
  using Result = AgentResult<std::string>;
  auto opened = co_await OpenSkillResource(id, ref, path, true);
  if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
  auto stream = std::move(opened).Value();
  std::string text;
  for (;;) {
    auto chunk = co_await stream.ReadAsync(16 * 1024);
    if (!chunk.Succeeded()) {
      const auto code = chunk.Error().code;
      co_return Fail<std::string>(code == IoErrorCode::PermissionDenied ? ErrorCode::PermissionDenied :
          code == IoErrorCode::TooLarge ? ErrorCode::BudgetExceeded : ErrorCode::ProviderFailure, "Skill resource could not be read");
    }
    if (chunk.Value().empty()) break;
    text.append(reinterpret_cast<const char*>(chunk.Value().data()), chunk.Value().size());
  }
  if (!ValidUtf8(text)) co_return Fail<std::string>(ErrorCode::ProtocolError, "Skill resource is not valid UTF-8 text");
  auto& run = runs.at(id);
  if (!RunOpen(*this, run)) co_return Fail<std::string>(ErrorCode::AlreadyClosed, "Run closed");
  run.model_skill_resources.push_back({std::move(ref), std::move(path), text.size()});
  co_return Result::Success(std::move(text));
}

Async<AsyncInputStream> RuntimeState::OpenSkillResource(RunId id, SkillRef ref, std::string path, bool model) {
  using Result = AgentResult<AsyncInputStream>;
  auto& run = runs.at(id);
  const auto& session = sessions.at(run.view.session);
  if (!RunOpen(*this, run)) co_return Fail<AsyncInputStream>(ErrorCode::AlreadyClosed, "Run closed");
  if (!ResourcePath(path)) co_return Fail<AsyncInputStream>(ErrorCode::InvalidArgument, "Skill resource path must be relative");
  const auto active = std::ranges::find(run.view.active_skills, ref, &SkillActivation::skill);
  if (active == run.view.active_skills.end()) co_return Fail<AsyncInputStream>(ErrorCode::PermissionDenied, "Skill version is not active");
  const auto activation = *active;
  const auto revision = access_revision;
  auto allowed = co_await CheckSkillAccess(run, activation, Confirmed(*this, run, activation));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto request = Request(run.principal, Capability::ReadSkillResource, &session, &run);
  request.actor.kind = model ? ActionParty::Kind::Agent : ActionParty::Kind::Host;
  request.skill = ref;
  request.skill_resource = path;
  allowed = co_await Require(request);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  if (model) {
    auto outbound = request;
    outbound.capability = Capability::SendToModel;
    outbound.recipient_model = run.view.model.id;
    allowed = co_await Require(std::move(outbound));
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  }
  auto package = co_await ReadSkillPackage(run, activation);
  if (!package.Succeeded()) co_return Result::Failure(package.Error());
  const auto& resources = package.Value().resources;
  const auto entry = std::ranges::find(resources, path, &SkillResource::relative_path);
  if (entry == resources.end()) co_return Fail<AsyncInputStream>(ErrorCode::NotFound, "Skill resource is not declared");
  if (model && !entry->media_type.starts_with("text/") && entry->media_type != "application/json")
    co_return Fail<AsyncInputStream>(ErrorCode::Unsupported, "Use the host Skill resource interface for non-text media");
  const auto limit = RunBudget(run).context_bytes.value_or(4 << 20);
  if (entry->size_bytes && *entry->size_bytes > limit)
    co_return Fail<AsyncInputStream>(ErrorCode::BudgetExceeded, "Skill resource exceeds the read budget");
  if (!RunOpen(*this, run) || revision != access_revision)
    co_return Fail<AsyncInputStream>(ErrorCode::Conflict, "Skill resource authorization changed");
  auto opened = Fail<AsyncInputStream>(ErrorCode::ProviderFailure, "Skill resource could not be opened");
  try {
    opened = co_await skill_sources.at(ref.source)->OpenResourceAsync(SourceRequest(request, run.stop.get_token()), ref, path);
  } catch (...) {
    ReportException(std::current_exception());
  }
  if (!opened.Succeeded()) co_return opened;
  if (!RunOpen(*this, run) || revision != access_revision)
    co_return Fail<AsyncInputStream>(ErrorCode::Conflict, "Skill resource authorization changed");
  auto source = std::make_shared<AsyncInputStream>(std::move(opened).Value());
  co_return Result::Success(AsyncInputStream::FromReader(
      [self = weak_from_this(), source = std::move(source), id, revision, remaining = limit](
          std::stop_token stop, std::size_t maximum_bytes) mutable -> Task<IoResult<Bytes>> {
        using Chunk = IoResult<Bytes>;
        auto state = self.lock();
        const auto permitted = [&] {
          if (!state || state->access_revision != revision || stop.stop_requested()) return false;
          const auto run = state->runs.find(id);
          return run != state->runs.end() && RunOpen(*state, run->second);
        };
        if (!permitted()) co_return Chunk::Failure({IoErrorCode::PermissionDenied, "Skill resource access changed"});
        auto read = co_await source->ReadAsync(std::min<std::uint64_t>(maximum_bytes, remaining ? remaining : 1));
        if (!permitted()) co_return Chunk::Failure({IoErrorCode::PermissionDenied, "Skill resource access changed"});
        if (!read.Succeeded()) co_return read;
        if (read.Value().size() > remaining) co_return Chunk::Failure({IoErrorCode::TooLarge, "Skill resource exceeds the read budget"});
        remaining -= read.Value().size();
        co_return read;
      }));
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

using detail::Fail;
using detail::RuntimeState;

AgentResult<void> AgentRuntime::RegisterSkillSource(SkillSourceId id, std::shared_ptr<SkillSource> source) {
  if (state_->stage != RuntimeState::Stage::Registering) return Fail<void>(ErrorCode::InvalidState, "Registration closed");
  if (!source || !detail::SkillIdentifier(id.value)) return Fail<void>(ErrorCode::InvalidArgument, "Invalid Skill source");
  if (!state_->skill_sources.emplace(std::move(id), std::move(source)).second)
    return Fail<void>(ErrorCode::Conflict, "Skill source ID already registered");
  return AgentResult<void>::Success();
}

Async<std::vector<SkillSummary>> AgentRuntime::ListSkillsAsync(SkillCatalogQuery query) {
  return [](std::shared_ptr<RuntimeState> state, SkillCatalogQuery query) -> Async<std::vector<SkillSummary>> {
    using Result = AgentResult<std::vector<SkillSummary>>;
    if (state->stage != RuntimeState::Stage::Started)
      co_return Fail<std::vector<SkillSummary>>(ErrorCode::RuntimeClosed, "Agent runtime closed");
    auto user = state->User();
    if (!user.Succeeded()) co_return Result::Failure(user.Error());
    PolicyRequest request;
    if (const auto* session = std::get_if<SessionId>(&query.target)) {
      if (query.project || query.workspace)
        co_return Fail<std::vector<SkillSummary>>(ErrorCode::InvalidArgument, "Use the session's existing project and workspace");
      auto access = co_await state->WithSession(*session, user.Value(), Capability::ReadSession);
      if (!access.Succeeded()) co_return Result::Failure(access.Error());
      request = RuntimeState::Request(user.Value(), Capability::DiscoverSkill, &state->sessions.at(*session));
    } else {
      auto agent = state->agents.find(std::get<AgentId>(query.target));
      if (agent == state->agents.end()) co_return Fail<std::vector<SkillSummary>>(ErrorCode::NotFound, "Assistant is unavailable");
      request = RuntimeState::Request(user.Value(), Capability::DiscoverSkill);
      request.agent = agent->second.ref;
      request.project = query.project;
      request.workspace = query.workspace;
    }
    co_return co_await state->DiscoverSkills(std::move(request), {});
  }(state_, std::move(query));
}

Async<std::vector<SkillSummary>> RunContext::ListSkillsAsync() {
  return [](RunContext context) -> Async<std::vector<SkillSummary>> {
    auto& state = *context.runtime_;
    auto& run = state.runs.at(context.run_);
    if (state.stage != RuntimeState::Stage::Started || detail::Terminal(run.view.state) || run.stop.stop_requested())
      co_return Fail<std::vector<SkillSummary>>(ErrorCode::AlreadyClosed, "Run closed");
    auto request = RuntimeState::Request(run.principal, Capability::DiscoverSkill, &state.sessions.at(run.view.session), &run);
    request.actor.kind = ActionParty::Kind::Host;
    co_return co_await state.DiscoverSkills(std::move(request), run.stop.get_token());
  }(*this);
}

std::vector<SkillActivation> RunContext::ActiveSkills() const {
  const auto run = runtime_->runs.find(run_);
  return run == runtime_->runs.end() ? std::vector<SkillActivation>{} : run->second.view.active_skills;
}

Async<std::vector<SkillResource>> RunContext::ActivateSkillAsync(SkillRef skill) {
  return [](RunContext context, SkillRef ref) -> Async<std::vector<SkillResource>> {
    SkillActivation request{std::move(ref), SkillActivationTrigger::Host};
    co_return co_await context.runtime_->ActivateSkill(context.run_, std::move(request));
  }(*this, std::move(skill));
}

Async<AsyncInputStream> RunContext::OpenSkillResourceAsync(SkillRef skill, std::string relative_path) {
  return [](RunContext context, SkillRef ref, std::string path) -> Async<AsyncInputStream> {
    co_return co_await context.runtime_->OpenSkillResource(context.run_, std::move(ref), std::move(path));
  }(*this, std::move(skill), std::move(relative_path));
}

Async<std::string> RunContext::ReadSkillTextForModelAsync(SkillRef skill, std::string relative_path) {
  return [](RunContext context, SkillRef ref, std::string path) -> Async<std::string> {
    co_return co_await context.runtime_->ReadSkillTextForModel(context.run_, std::move(ref), std::move(path));
  }(*this, std::move(skill), std::move(relative_path));
}

} // namespace huxerui::agentkit
