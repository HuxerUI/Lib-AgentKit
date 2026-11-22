#include "local_files.h"

#include <app_resources.h>

#include "preview_json.h"
#include "run_settings.h"

#include <huxerui/task.h>
#include <huxerui/huxerui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using huxerui::File;

enum class Operation { List, Find, Read, MakeDirectory, Create, Edit, Copy, Move, Remove };

struct FileTool {
  Operation operation;
  const char* name;
  const char* description;
  huxerui::StringResource label;
};

const std::array tools{
    FileTool{Operation::List, "list_directory", "List entries in a local directory by path.", app::strings::ui_local_list_directory},
    FileTool{Operation::Find, "find_files", "Search within a local directory by file name or literal text.", app::strings::ui_local_find_files},
    FileTool{Operation::Read, "read_file", "Read UTF-8 text from a local file. Line numbers start at 1.", app::strings::ui_local_read_file},
    FileTool{Operation::MakeDirectory, "create_directory", "Create one directory. Its parent must exist.", app::strings::ui_local_create_directory},
    FileTool{Operation::Create, "create_file", "Create a new UTF-8 file. Fails if the path already exists.", app::strings::ui_local_create_file},
    FileTool{Operation::Edit, "edit_file", "Replace one exact occurrence of old_text with new_text in an existing UTF-8 file.",
        app::strings::ui_local_edit_file},
    FileTool{Operation::Copy, "copy_file", "Copy a regular file to a new path without overwriting.", app::strings::ui_local_copy_file},
    FileTool{Operation::Move, "move_file", "Move a regular file to a new path without overwriting.", app::strings::ui_local_move_file},
    FileTool{Operation::Remove, "delete_file", "Delete a regular file or an empty directory.", app::strings::ui_local_delete_file},
};

constexpr std::size_t max_text = 512 * 1024;
constexpr std::size_t max_entries = 2048;
constexpr std::size_t max_matches = 100;
const kit::ToolId choose_directory{"choose_directory"};
const kit::ToolId task_directory{"task_directory"};

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

std::string TextPath(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

const FileTool* FindTool(const kit::ToolId& id) {
  const auto found = std::ranges::find_if(tools, [&](const auto& tool) { return id.value == tool.name; });
  return found == tools.end() ? nullptr : &*found;
}

bool Writes(Operation operation) {
  return operation >= Operation::MakeDirectory;
}

bool UsesDestination(Operation operation) {
  return operation == Operation::Copy || operation == Operation::Move;
}

bool WritesSource(Operation operation) {
  return Writes(operation) && operation != Operation::Copy;
}

bool Link(const fs::path& path, fs::file_status status) {
  if (fs::is_symlink(status)) return true;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

bool Within(const fs::path& root, const fs::path& path) {
  return std::ranges::mismatch(root, path).in1 == root.end();
}

kit::AgentResult<fs::path> ResolvePath(std::string_view text, const std::optional<File>& directory = {}) {
  using Result = kit::AgentResult<fs::path>;
  if (text.empty() || text.size() > 4096 || text.find('\0') != text.npos)
    return Fail<fs::path>(kit::ErrorCode::InvalidArgument, "Provide a local file path");
  auto path = fs::u8path(text);
  if (!path.is_absolute()) {
    if (!directory || path.has_root_path())
      return Fail<fs::path>(kit::ErrorCode::InvalidArgument, "Use an absolute path or choose a working directory");
    path = fs::u8path(directory->Path()) / path;
  }
#ifdef _WIN32
  const auto drive = path.root_name().native();
  if (drive.size() != 2 || drive.back() != L':')
    return Fail<fs::path>(kit::ErrorCode::Unsupported, "Use a local drive path");
#endif
  auto current = path.root_path();
  for (const auto& part : path.relative_path()) {
    if (part.empty() || part == ".") continue;
    const auto component = TextPath(part);
    if (part == ".." || component.find(':') != component.npos || component.ends_with('.') || component.ends_with(' '))
      return Fail<fs::path>(kit::ErrorCode::InvalidArgument, "Use a normalized local file path");
    current /= part;
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    if (Link(current, status))
      return Fail<fs::path>(kit::ErrorCode::PermissionDenied, "Use the actual path instead of a link or junction");
    if (error && error != std::errc::no_such_file_or_directory)
      return Fail<fs::path>(kit::ErrorCode::PermissionDenied, "The system could not resolve the path");
  }
  std::error_code error;
  auto resolved = fs::weakly_canonical(current, error);
  return error ? Fail<fs::path>(kit::ErrorCode::PermissionDenied, "The system could not resolve the path")
               : Result::Success(std::move(resolved));
}

std::optional<File> DefaultDirectory(const CodeWorkspaces& workspaces, const std::optional<kit::WorkspaceId>& id) {
  const auto workspace = id ? workspaces.Find(*id) : std::nullopt;
  return workspace ? workspace->NativeFolder() : std::nullopt;
}

std::optional<kit::ResourceRef> FileVersion(const fs::path& path, const kit::ResourceId& id, std::error_code& error) {
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) error.clear();
  if (error || !fs::exists(status)) return {};
  if (!fs::is_regular_file(status) && !fs::is_directory(status)) {
    error = std::make_error_code(std::errc::operation_not_supported);
    return {};
  }
  const auto modified = fs::last_write_time(path, error);
  if (error) return {};
  const auto size = fs::is_regular_file(status) ? fs::file_size(path, error) : 0;
  if (error) return {};
  return kit::ResourceRef{id, "local:" + std::to_string(modified.time_since_epoch().count()) + ":" + std::to_string(size)};
}

kit::JsonSchema InputSchema(Operation operation) {
  const auto text = Json{{"type", "string"}};
  Json schema{{"type", "object"}, {"properties", {{"path", text}}}, {"required", {"path"}}, {"additionalProperties", false}};
  const auto field = [&](const char* name, const Json& type, bool required = true) {
    schema["properties"][name] = type;
    if (required) schema["required"].push_back(name);
  };
  if (UsesDestination(operation)) field("destination", text);
  if (operation == Operation::Create) field("content", text);
  if (operation == Operation::Edit) {
    field("old_text", text);
    field("new_text", text);
  }
  if (operation == Operation::Find) {
    field("query", text);
    field("match_contents", {{"type", "boolean"}}, false);
  }
  if (operation == Operation::Read) {
    const auto line = Json{{"type", "integer"}, {"minimum", 1}, {"maximum", static_cast<int>(max_text)}};
    field("start_line", line, false);
    field("end_line", line, false);
  }
  return {DecodeJson(std::move(schema))};
}

std::string ReadText(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Could not open the file");
  std::string text(max_text + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  const auto count = static_cast<std::size_t>(input.gcount());
  if (input.bad() || count > max_text) throw std::runtime_error("File exceeds the text read limit");
  text.resize(count);
  if (text.find('\0') != text.npos) throw std::runtime_error("Use a UTF-8 text file");
  try { static_cast<void>(Json(text).dump()); }
  catch (const Json::exception&) { throw std::runtime_error("Use a UTF-8 text file"); }
  return text;
}

Json FindEntries(Operation operation, const fs::path& directory, const Json& args,
    const std::function<bool()>& interrupted) {
  const bool search = operation == Operation::Find;
  const auto query = args.value("query", std::string{});
  const bool contents = args.value("match_contents", false);
  Json entries = Json::array();
  std::vector<fs::path> pending{directory};
  std::size_t visited = 0;
  bool truncated = false;
  while (!pending.empty() && !interrupted() && !truncated) {
    auto next = std::move(pending.back());
    pending.pop_back();
    std::error_code error;
    auto cursor = fs::directory_iterator(next, error);
    if (error && next == directory) throw std::runtime_error("Could not open the directory");
    for (; !error && cursor != fs::directory_iterator{}; cursor.increment(error)) {
      if (interrupted()) break;
      if (++visited > max_entries || entries.size() >= max_matches) { truncated = true; break; }
      const auto path = cursor->path();
      const auto status = cursor->symlink_status(error);
      if (error) break;
      if (Link(path, status) || (!fs::is_regular_file(status) && !fs::is_directory(status))) continue;
      const auto name = TextPath(path.filename());
      if (search && fs::is_directory(status)) pending.push_back(path);
      if (search && contents) {
        if (!fs::is_regular_file(status)) continue;
        try {
          std::istringstream lines(ReadText(path));
          std::string line;
          for (std::size_t number = 1; std::getline(lines, line); ++number) {
            if (line.find(query) == line.npos) continue;
            entries.push_back({{"path", TextPath(path)}, {"line", number}});
            break;
          }
        } catch (const std::runtime_error&) {
          continue;
        }
      } else if (!search || name.find(query) != name.npos) {
        entries.push_back({{"path", TextPath(path)}, {"directory", fs::is_directory(status)}});
      }
    }
  }
  return {{"entries", std::move(entries)}, {"truncated", truncated}};
}

Json ReadLines(const fs::path& path, const Json& args) {
  const auto begin = args.value("start_line", 1);
  const auto end = args.value("end_line", begin + 499);
  if (end < begin) throw std::runtime_error("The last line precedes the first line");
  const auto text = ReadText(path);
  std::string result;
  int last = begin - 1;
  std::size_t offset = 0;
  for (int number = 1; number <= end && offset < text.size(); ++number) {
    const auto newline = text.find('\n', offset);
    const auto next = newline == text.npos ? text.size() : newline + 1;
    if (number >= begin) {
      if (result.size() + next - offset > 64 * 1024) break;
      result.append(text, offset, next - offset);
      last = number;
    }
    offset = next;
  }
  return {{"path", TextPath(path)}, {"text", std::move(result)}, {"start_line", begin},
      {"end_line", last}, {"truncated", offset != text.size()}};
}

std::string DiffLinePreview(std::string_view text, char prefix) {
  std::string result;
  const auto clip = text.substr(0, std::min<std::size_t>(text.size(), 4096));
  auto remaining = clip;
  while (!remaining.empty()) {
    const auto newline = remaining.find('\n');
    result += prefix;
    result += remaining.substr(0, newline);
    result += '\n';
    if (newline == remaining.npos) break;
    remaining.remove_prefix(newline + 1);
  }
  if (text.size() > clip.size()) {
    while (!result.empty()) {
      try { static_cast<void>(Json(result).dump()); break; } catch (const Json::exception&) { result.pop_back(); }
    }
    result += "\n...";
  }
  return result;
}

kit::AgentResult<kit::PreparedChange> Prepare(FileTool tool, const kit::ToolPrepareContext& context, const Json& args,
    const LocalFilePermissions& permissions) {
  using Result = kit::AgentResult<kit::PreparedChange>;
  if (context.readable.size() != (UsesDestination(tool.operation) ? 2 : 1))
    return Fail<kit::PreparedChange>(kit::ErrorCode::PermissionDenied, "The target is outside the available scope");
  const auto& source = context.readable.front();
  const bool new_path = tool.operation == Operation::Create || tool.operation == Operation::MakeDirectory;
  if ((new_path && source.current) || (UsesDestination(tool.operation) && context.readable.back().current))
    return Fail<kit::PreparedChange>(kit::ErrorCode::Conflict, "The destination already exists");
  auto title = huxerui::UseService<huxerui::Resources>()->GetString(tool.label);
  auto change = kit::PreparedChange{.preview = {.title = std::move(title), .summary = source.resource.value},
      .execution_data = DecodeJson(Json{{"access_revision", permissions.revision}, {"source", source.resource.value},
          {"source_version", source.current ? Json(source.current->version) : Json(nullptr)}}), .undo_support = kit::UndoSupport::None};
  if (UsesDestination(tool.operation)) change.preview.summary += "\nTo " + context.readable.back().resource.value;
  if (tool.operation == Operation::Create || tool.operation == Operation::Edit) {
    const auto before = args.value("old_text", std::string{});
    const auto after = args.value(tool.operation == Operation::Create ? "content" : "new_text", std::string{});
    if ((tool.operation == Operation::Edit && before.empty()) || before.size() > max_text || after.size() > max_text ||
        before.find('\0') != before.npos || after.find('\0') != after.npos)
      return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "Provide a valid, bounded text change");
    change.preview.summary += "\n" + DiffLinePreview(before, '-') + DiffLinePreview(after, '+');
  }
  if (tool.operation == Operation::Find && args.value("query", std::string{}).empty())
    return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "Provide a search query");
  for (const auto& grant : context.readable) {
    if (tool.operation == Operation::Copy && grant.resource == source.resource) continue;
    change.targets.push_back({grant.resource, grant.current});
  }
  return Result::Success(std::move(change));
}

bool MoveNewFile(const fs::path& source, const fs::path& destination, std::error_code& error) {
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(), 0)) return true;
  error = {static_cast<int>(GetLastError()), std::system_category()};
  return false;
#else
  fs::create_hard_link(source, destination, error);
  return !error && fs::remove(source, error);
#endif
}

bool WriteNewText(const fs::path& path, std::string_view text, bool overwrite, std::error_code& error) {
  const auto name = kit::NewCommandId().value + ".ganwork-temp";
  const auto stage = path.parent_path() / fs::u8path(name);
  const auto temp = File(stage.u8string());
  if (temp.WriteString(text)) {
    if (!overwrite && MoveNewFile(stage, path, error)) return true;
    if (overwrite) {
#ifdef _WIN32
      if (ReplaceFileW(path.c_str(), stage.c_str(), nullptr, 0, nullptr, nullptr)) return true;
      error = {static_cast<int>(GetLastError()), std::system_category()};
#else
      fs::rename(stage, path, error);
      if (!error) return true;
#endif
    }
  }
  static_cast<void>(temp.Delete());
  return false;
}

kit::AgentResult<kit::ToolExecutionResult> Execute(FileTool tool, kit::PreparedToolCall call,
    std::vector<kit::ResourceAccess> grants, std::stop_token stop, const std::shared_ptr<LocalFileAccess>& access) {
  using Result = kit::AgentResult<kit::ToolExecutionResult>;
  const auto failure = [](kit::ErrorCode code, const std::string& message) {
    return Result::Success({.outcome = kit::ToolOutcome::Failed, .reason = kit::AgentError{.code = code, .message = message}});
  };
  const auto prepared = EncodeJson(call.change.execution_data);
  const auto revision = prepared.at("access_revision").get<std::string>();
  if (stop.stop_requested()) return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
  if (access->Current()->revision != revision)
    return failure(kit::ErrorCode::PermissionDenied, "File access changed. Request the operation again");
  if (grants.size() != (UsesDestination(tool.operation) ? 2 : 1))
    return failure(kit::ErrorCode::PermissionDenied, "The target is outside the available scope");
  const auto& from = grants.front();
  if (prepared.at("source") != from.resource.value ||
      prepared.at("source_version") != (from.current ? Json(from.current->version) : Json(nullptr)))
    return failure(kit::ErrorCode::Conflict, "The source changed after the plan was prepared");
  for (const auto& grant : grants) {
    auto path = ResolvePath(grant.resource.value);
    std::error_code error;
    if (!path.Succeeded() || TextPath(path.Value()) != grant.resource.value ||
        FileVersion(path.Value(), grant.resource, error) != grant.current || error)
      return failure(kit::ErrorCode::Conflict, "The target changed before execution");
  }
  const auto operation = tool.operation;
  const auto source = fs::u8path(grants.front().resource.value);
  const auto destination = UsesDestination(operation) ? fs::u8path(grants.back().resource.value) : fs::path{};
  const auto args = EncodeJson(call.request.arguments);
  const auto interrupted = [&] {
    return stop.stop_requested() || access->Current()->revision != revision;
  };
  try {
    if (!Writes(operation)) {
      auto value = operation == Operation::Read ? ReadLines(source, args) : FindEntries(operation, source, args, interrupted);
      if (stop.stop_requested()) return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
      if (access->Current()->revision != revision)
        return failure(kit::ErrorCode::PermissionDenied, "File access changed during the read");
      return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .model_value = DecodeJson(std::move(value))});
    }
    std::optional<std::string> edit;
    if (operation == Operation::Edit) {
      edit = ReadText(source);
      const auto before = args.at("old_text").get<std::string>();
      const auto at = edit->find(before);
      if (at == edit->npos || edit->find(before, at + 1) != edit->npos)
        return failure(kit::ErrorCode::Conflict, "The old text must match exactly once");
      edit->replace(at, before.size(), args.at("new_text").get<std::string>());
      if (edit->size() > max_text) return failure(kit::ErrorCode::InvalidArgument, "The edited file would exceed the text limit");
    }
    if (stop.stop_requested()) return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
    if (access->Current()->revision != revision)
      return failure(kit::ErrorCode::PermissionDenied, "File access changed. Request the operation again");
    std::error_code error;
    bool succeeded = false;
    switch (operation) {
    case Operation::MakeDirectory: succeeded = fs::create_directory(source, error); break;
    case Operation::Create: succeeded = WriteNewText(source, args.at("content").get<std::string>(), false, error); break;
    case Operation::Edit: succeeded = WriteNewText(source, *edit, true, error); break;
    case Operation::Copy: succeeded = fs::copy_file(source, destination, fs::copy_options::none, error); break;
    case Operation::Move:
      succeeded = fs::is_regular_file(source, error) && MoveNewFile(source, destination, error);
      break;
    case Operation::Remove: succeeded = fs::remove(source, error); break;
    default: break;
    }
    auto result = kit::ToolExecutionResult{.outcome = succeeded ? kit::ToolOutcome::Succeeded : kit::ToolOutcome::Failed};
    if (succeeded) {
      const auto& output = destination.empty() ? source : destination;
      result.model_value = DecodeJson(Json{{"path", TextPath(output)}, {"completed", true}});
      result.blocks.push_back({{kit::NewCommandId().value}, kit::TextBlock{call.change.preview.title + ": " + TextPath(output), false}});
    } else {
      result.reason = kit::AgentError{.code = kit::ErrorCode::ProviderFailure,
          .message = error ? "The file operation failed: " + error.message() : "The file operation failed"};
    }
    for (const auto& grant : grants) {
      if (grant.mode != kit::ResourceAccessMode::Write) continue;
      error.clear();
      const auto after = FileVersion(fs::u8path(grant.resource.value), grant.resource, error);
      if (error) {
        result.outcome = kit::ToolOutcome::ResultUnknown;
        result.effects.push_back({grant.resource, kit::ResourceEffectState::Unknown, grant.current});
      } else if (after != grant.current) {
        if (!succeeded) result.outcome = kit::ToolOutcome::PartiallyApplied;
        result.effects.push_back({grant.resource, kit::ResourceEffectState::Applied, grant.current, after});
      }
    }
    return Result::Success(std::move(result));
  } catch (const std::runtime_error& error) {
    return failure(kit::ErrorCode::ProviderFailure, error.what());
  }
}

} // namespace

LocalFileAccess::LocalFileAccess(File temporary_directory)
    : task_files_(temporary_directory.Child("ganwork-task-files")) {}

void LocalFileAccess::Apply(LocalFilePermissions permissions) {
  current_.store(std::make_shared<const LocalFilePermissions>(std::move(permissions)));
}

std::shared_ptr<const LocalFilePermissions> LocalFileAccess::Current() const {
  return current_.load();
}

void LocalFileAccess::SetDirectoryPicker(std::weak_ptr<huxerui::FilePicker> picker) {
  picker_ = std::move(picker);
}

std::shared_ptr<huxerui::FilePicker> LocalFileAccess::DirectoryPicker() const {
  return picker_.lock();
}

File LocalFileAccess::TaskDirectory(const kit::RunId& run) const {
  return task_files_.Child(run.value);
}

kit::PolicyDecision LocalFileAccess::Decide(const kit::PolicyRequest& request) const {
  using kit::AccessDecision;
  if (request.actor.principal != PreviewUser) return {AccessDecision::Deny, "File access belongs to another user"};
  if (request.capability != kit::Capability::ExecuteTool || !request.invocation) return {AccessDecision::Allow, {}};
  if (request.tool && request.tool->id == choose_directory) return {AccessDecision::RequireConfirmation, {}};
  if (request.tool && request.tool->id == task_directory) return {AccessDecision::Allow, {}};
  const auto permissions = Current();
  if (permissions->full_access) return {AccessDecision::Allow, {}};
  const auto& invocation = *request.invocation;
  const auto scratch = ResolvePath(TaskDirectory(invocation.run).Path());
  if (scratch.Succeeded() && !invocation.prepared_targets.empty() &&
      std::ranges::all_of(invocation.prepared_targets, [&](const auto& target) {
        return Within(scratch.Value(), fs::u8path(target.target.value));
      })) {
    const auto* tool = FindTool(invocation.tool.id);
    if (!tool || tool->operation != Operation::Copy) return {AccessDecision::Allow, {}};
    const auto source = ResolvePath(EncodeJson(invocation.arguments).at("path").get<std::string>());
    if (source.Succeeded() && Within(scratch.Value(), source.Value())) return {AccessDecision::Allow, {}};
  }
  if (invocation.effect == kit::ToolEffect::Read && !invocation.prepared_targets.empty()) {
    const auto covered = std::ranges::all_of(invocation.prepared_targets, [&](const auto& target) {
      return std::ranges::any_of(permissions->directories, [&](const auto& directory) {
        const auto root = LocalDirectory(File(directory));
        return root.Succeeded() && Within(fs::u8path(root.Value()), fs::u8path(target.target.value));
      });
    });
    if (covered) return {AccessDecision::Allow, {}};
  }
  return {AccessDecision::RequireConfirmation, {}};
}

bool IsLocalFileTool(const kit::ToolId& id) {
  return id == choose_directory || id == task_directory || FindTool(id) != nullptr;
}

std::optional<huxerui::StringResource> LocalFileToolLabel(const kit::ToolId& id) {
  if (id == choose_directory) return app::strings::ui_local_choose_directory;
  if (id == task_directory) return app::strings::ui_local_task_directory;
  const auto* tool = FindTool(id);
  return tool ? std::optional{tool->label} : std::nullopt;
}

std::vector<kit::ToolId> LocalFileTools() {
  std::vector<kit::ToolId> ids;
  for (const auto& tool : tools) ids.push_back({tool.name});
  ids.push_back(choose_directory);
  ids.push_back(task_directory);
  return ids;
}

kit::AgentResult<std::string> LocalDirectory(const File& directory) {
  auto resolved = ResolvePath(directory.Path());
  if (!resolved.Succeeded()) return kit::AgentResult<std::string>::Failure(resolved.Error());
  std::error_code error;
  if (!fs::is_directory(resolved.Value(), error) || error)
    return Fail<std::string>(kit::ErrorCode::InvalidArgument, "Choose a local directory that exists");
  return kit::AgentResult<std::string>::Success(TextPath(resolved.Value()));
}

kit::ToolResourceResolver LocalFileResources(std::shared_ptr<CodeWorkspaces> workspaces) {
  return [workspaces = std::move(workspaces)](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
    using Grants = std::vector<kit::ResourceAccess>;
    using Result = kit::AgentResult<Grants>;
    if (invocation.tool.id == choose_directory || invocation.tool.id == task_directory) co_return Result::Success({});
    const auto tool = FindTool(invocation.tool.id);
    if (!tool) co_return Fail<Grants>(kit::ErrorCode::InvalidArgument, "Unknown local file tool");
    const auto args = EncodeJson(invocation.arguments);
    const auto directory = DefaultDirectory(*workspaces, invocation.workspace);
    const bool execute = invocation.phase == kit::ToolAccessPhase::Execute;
    const auto add = [&](Grants& grants, const char* field, bool write) -> kit::AgentResult<void> {
      auto path = ResolvePath(args.at(field).get<std::string>(), directory);
      if (!path.Succeeded()) return kit::AgentResult<void>::Failure(path.Error());
      kit::ResourceId id{TextPath(path.Value())};
      if (std::ranges::find(grants, id, &kit::ResourceAccess::resource) != grants.end())
        return Fail<void>(kit::ErrorCode::InvalidArgument, "The source and destination must differ");
      std::error_code error;
      auto version = FileVersion(path.Value(), id, error);
      if (error) return Fail<void>(kit::ErrorCode::PermissionDenied, "Could not inspect the file target");
      grants.push_back({std::move(id), std::move(version), execute && write ? kit::ResourceAccessMode::Write : kit::ResourceAccessMode::Read});
      return kit::AgentResult<void>::Success();
    };
    Grants grants;
    auto source = add(grants, "path", WritesSource(tool->operation));
    if (!source.Succeeded()) co_return Result::Failure(source.Error());
    if (UsesDestination(tool->operation)) {
      auto destination = add(grants, "destination", true);
      if (!destination.Succeeded()) co_return Result::Failure(destination.Error());
    }
    co_return Result::Success(std::move(grants));
  };
}

kit::AgentResult<void> RegisterLocalFileTools(const std::shared_ptr<kit::AgentRuntime>& runtime, std::shared_ptr<LocalFileAccess> access) {
  for (const auto& tool : tools) {
    auto result = runtime->RegisterTool({.id = {tool.name}, .version = 1, .description = tool.description,
        .input_schema = InputSchema(tool.operation), .effect = Writes(tool.operation) ? kit::ToolEffect::WriteResource : kit::ToolEffect::Read,
        .prepare = [tool, access](kit::ToolPrepareContext context, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
          co_return Prepare(tool, context, EncodeJson(request.arguments), *access->Current());
        },
        .execute = [tool, access](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
          co_return co_await huxerui::RunWorker(Execute, tool, std::move(call), context.Resources(), context.StopToken(), access);
        }});
    if (!result.Succeeded()) return result;
  }
  auto choose = runtime->RegisterTool({.id = choose_directory, .version = 1,
      .description = "Ask the user to pick the local working directory when the task's destination is ambiguous. "
          "Returns its absolute path without changing the conversation's project or permissions.",
      .input_schema = {DecodeJson(Json{{"type", "object"}, {"properties", {{"reason", {{"type", "string"}}}}},
          {"required", {"reason"}}, {"additionalProperties", false}})},
      .effect = kit::ToolEffect::Read,
      .prepare = [access](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        const auto args = EncodeJson(request.arguments);
        const auto reason = args.at("reason").get<std::string>();
        if (reason.empty() || reason.size() > 1024)
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "Explain which directory is needed");
        co_return kit::AgentResult<kit::PreparedChange>::Success({
            .preview = {.title = huxerui::UseService<huxerui::Resources>()->GetString(app::strings::ui_local_choose_directory),
                        .summary = reason},
            .execution_data = kit::JsonValue{access->Current()->revision},
            .undo_support = kit::UndoSupport::None});
      },
      .execute = [access](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        using Result = kit::AgentResult<kit::ToolExecutionResult>;
        const auto revision = std::get<std::string>(call.change.execution_data.value);
        if (context.StopToken().stop_requested() || access->Current()->revision != revision)
          co_return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
        const auto picker = access->DirectoryPicker();
        if (!picker || !picker->CanOpenDirectories())
          co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::Unsupported, "Open the GanWork window to select a directory");
        auto selected = co_await picker->OpenDirectoryAsync();
        if (!selected || context.StopToken().stop_requested() || access->Current()->revision != revision)
          co_return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
        auto file = selected->AsFile();
        if (!file) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::Unsupported, "Choose a local directory");
        auto folder = LocalDirectory(*file);
        if (!folder.Succeeded()) co_return Result::Failure(folder.Error());
        co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded,
            .model_value = DecodeJson(Json{{"path", std::move(folder).Value()}})});
      }});
  if (!choose.Succeeded()) return choose;
  return runtime->RegisterTool({.id = task_directory, .version = 1,
      .description = "Get this run's private temporary directory for files with no requested destination. "
          "Returns an absolute directory path.",
      .input_schema = {DecodeJson(Json{{"type", "object"}, {"additionalProperties", false}})},
      .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext context, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        if (!context.actor.run) co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidState, "A run is required");
        co_return kit::AgentResult<kit::PreparedChange>::Success({
            .preview = {.title = huxerui::UseService<huxerui::Resources>()->GetString(app::strings::ui_local_task_directory)},
            .execution_data = kit::JsonValue{context.actor.run->value}, .undo_support = kit::UndoSupport::None});
      },
      .execute = [access](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        using Result = kit::AgentResult<kit::ToolExecutionResult>;
        if (context.StopToken().stop_requested()) co_return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
        const auto scratch = access->TaskDirectory({std::get<std::string>(call.change.execution_data.value)});
        auto safe = ResolvePath(scratch.Path());
        if (!safe.Succeeded()) co_return Result::Failure(safe.Error());
        if (!co_await scratch.CreateDirectoriesAsync())
          co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::ProviderFailure, "Could not open the temporary directory");
        co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded,
            .model_value = DecodeJson(Json{{"path", TextPath(safe.Value())}})});
      }});
}

} // namespace agentkit_preview
