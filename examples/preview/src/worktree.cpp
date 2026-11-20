#include "worktree.h"

#include "git_process.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
namespace fs = std::filesystem;
using huxerui::File;
using Json = nlohmann::json;
using namespace std::chrono_literals;

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

std::string Utf8(const fs::path& path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}

std::string Unline(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

bool Identifier(std::string_view value) {
  return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](char ch) {
    return (ch >= '0' && ch <= '9') || ch == '-';
  });
}

class PlanLock {
public:
#if defined(_WIN32)
  explicit PlanLock(HANDLE file) : file_(file) {}

  ~PlanLock() { CloseHandle(file_); }
private:
  HANDLE file_;
#elif (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)
  explicit PlanLock(int file) : file_(file) {}

  ~PlanLock() { close(file_); }
private:
  int file_;
#endif
};

kit::AgentResult<std::shared_ptr<PlanLock>> LockPlan(const File& storage, const kit::WorkspaceCopyPlanId& id) {
  using Result = kit::AgentResult<std::shared_ptr<PlanLock>>;
  const auto path = fs::u8path(storage.Child(id.value + ".lock").Path());
#if defined(_WIN32)
  const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file != INVALID_HANDLE_VALUE) return Result::Success(std::make_shared<PlanLock>(file));
  const auto error = GetLastError();
  const bool occupied = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
#elif (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)
  const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  bool occupied = false;
  if (file >= 0) {
    if (flock(file, LOCK_EX | LOCK_NB) == 0) return Result::Success(std::make_shared<PlanLock>(file));
    occupied = errno == EWOULDBLOCK;
    close(file);
  }
#else
  constexpr bool occupied = false;
#endif
  return Fail<std::shared_ptr<PlanLock>>(occupied ? kit::ErrorCode::Busy : kit::ErrorCode::PersistenceFailure,
      occupied ? "The worktree copy is being handled in another window" : "Could not lock the worktree plan");
}

bool GitInput(std::string_view value, std::size_t max = 2048) {
  return !value.empty() && value.size() <= max && value.find_first_of("\r\n") == value.npos && value.find('\0') == value.npos;
}

bool Descendant(const fs::path& path, const fs::path& parent) {
  const auto relative = path.lexically_relative(parent);
  return !relative.empty() && *relative.begin() != "..";
}

std::optional<fs::path> Canonical(const fs::path& path) {
  std::error_code error;
  auto result = fs::canonical(path, error);
  return error ? std::nullopt : std::optional{std::move(result)};
}

std::optional<fs::path> NormalDirectory(const fs::path& path) {
  if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..") return {};
  auto parent = Canonical(path.parent_path());
  if (!parent) return {};
  std::error_code error;
  if (!fs::is_directory(*parent, error)) return {};
  return *parent / path.filename();
}

bool Occupied(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  return status.type() != fs::file_type::not_found || (error && error != std::errc::no_such_file_or_directory);
}

kit::AgentResult<std::string> GitValue(const File& storage, const fs::path& repo, std::vector<std::string> arguments) {
  using Result = kit::AgentResult<std::string>;
  auto result = RunGit(storage, repo, std::move(arguments));
  if (!result.Succeeded()) return Result::Failure(result.Error());
  auto output = Unline(std::move(result.Value().output));
  if (result.Value().exit_code != 0)
    return Fail<std::string>(kit::ErrorCode::Conflict, output.empty() ? "Git rejected the worktree operation" : std::move(output));
  return Result::Success(std::move(output));
}

Json Boundary(const kit::HistoryBoundary& boundary) {
  return {{"session", boundary.session.value}, {"kind", boundary.anchor.index()},
      {"anchor", std::visit([](const auto& id) { return id.value; }, boundary.anchor)}, {"through", boundary.through_sequence}};
}

Json Project(const std::optional<kit::ProjectId>& project) {
  return project ? Json(project->value) : Json(nullptr);
}

std::int64_t Now() {
  return std::chrono::duration_cast<kit::Duration>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<Json> ReadRecord(const File& file) {
  auto data = file.Stat();
  if (!data.Succeeded() || data.Value().size > 64 * 1024) return {};
  auto text = file.ReadString();
  if (!text.Succeeded()) return {};
  auto parsed = Json::parse(text.Value(), nullptr, false);
  if (!parsed.is_object()) return {};
  return parsed;
}

WorktreePreview ReadPreview(std::string id, const Json& record) {
  const auto text = [&](const char* key) { return record.at(key).get<std::string>(); };
  const auto& message = record.at("boundary");
  auto anchor = message.at("anchor").get<std::string>();
  const auto project = record.at("project").is_null() ? std::nullopt :
      std::optional{kit::ProjectId{text("project")}};
  kit::HistoryBoundary boundary{{message.at("session").get<std::string>()},
      message.at("kind") == 0 ? std::variant<kit::MessageId, kit::RunId>{kit::MessageId{std::move(anchor)}} :
          std::variant<kit::MessageId, kit::RunId>{kit::RunId{std::move(anchor)}},
      message.at("through").get<kit::Sequence>()};
  WorktreePreview result{{{std::move(id)}, 1}, text("repo"), text("commit"), text("directory"), text("branch"), {text("command")}};
  result.request = {.from = std::move(boundary), .options = {.agent = CodeAgent, .project = project, .title = text("title")},
      .workspace = kit::WorkspaceChoice{kit::WorkspaceChoice::Kind::CopyFromSource, {text("workspace")}, result.plan}};
  if (record.contains("comparison")) result.comparison = kit::CommandId{text("comparison")};
  return result;
}

std::vector<WorktreePreview> Pending(const File& storage, const kit::PrincipalId& principal) {
  std::vector<WorktreePreview> result;
  std::error_code error;
  const auto entries = fs::directory_iterator(fs::u8path(storage.Path()), error);
  for (const auto& entry : entries) {
    if (entry.path().extension() != ".owner") continue;
    auto id = entry.path().stem().string();
    if (!Identifier(id) || storage.Child(id + ".settled").Exists() || storage.Child(id + ".discarded").Exists()) continue;
    auto plan = ReadRecord(storage.Child(id + ".plan"));
    auto owner = ReadRecord(File(entry.path().u8string()));
    if (!plan || !owner || !plan->contains("principal") ||
        !plan->contains("command") || !owner->contains("command") ||
        plan->at("principal") != principal.value ||
        (!plan->contains("comparison") && plan->at("command") != owner->at("command"))) continue;
    try {
      auto preview = ReadPreview(id, *plan);
      preview.cleanup_started = storage.Child(id + ".discarding").Exists();
      result.push_back(std::move(preview));
    } catch (const Json::exception&) {}
  }
  return result;
}

bool Publish(const File& storage, const File& output, const Json& record) {
  const auto draft = storage.Child(kit::NewCommandId().value + ".pending");
  const auto written = draft.WriteString(record.dump());
  if (written && draft.MoveTo(output)) return true;
  static_cast<void>(draft.Delete());
  return false;
}

struct Checkout {
  std::string head;
  std::string branch;
  std::string lock;
};

kit::AgentResult<std::optional<Checkout>> Inspect(const File& storage, const fs::path& repo, const fs::path& destination) {
  using Result = kit::AgentResult<std::optional<Checkout>>;
  auto list = RunGit(storage, repo, {"worktree", "list", "--porcelain", "-z"});
  if (!list.Succeeded()) return Result::Failure(list.Error());
  if (list.Value().exit_code != 0) return Fail<std::optional<Checkout>>(kit::ErrorCode::Conflict, "Could not read registered worktrees");

  std::optional<Checkout> found;
  std::string_view data(list.Value().output);
  while (!data.empty()) {
    const auto end = data.find('\0');
    if (end == data.npos) return Fail<std::optional<Checkout>>(kit::ErrorCode::Conflict, "Invalid Git worktree list");
    const auto line = data.substr(0, end);
    data.remove_prefix(end + 1);
    if (line.starts_with("worktree ")) {
      auto path = fs::u8path(line.substr(9));
      if (path.lexically_normal() == destination || Canonical(path) == destination) found.emplace();
    } else if (found) {
      if (line.empty()) return Result::Success(std::move(found));
      if (line.starts_with("HEAD ")) found->head = line.substr(5);
      if (line.starts_with("branch ")) found->branch = line.substr(7);
      if (line.starts_with("locked ")) found->lock = line.substr(7);
    }
  }
  return Result::Success(std::move(found));
}

kit::AgentResult<WorktreePreview> Prepare(const File& storage, CodeWorkspace source, kit::PrincipalId principal,
    kit::HistoryBoundary boundary, std::optional<kit::ProjectId> project, std::string title, std::string revision,
    std::string proposed_directory, std::string branch, const std::optional<kit::CommandId>& comparison) {
  using Result = kit::AgentResult<WorktreePreview>;
  if (!GitInput(revision, 256) || revision.starts_with('-') || !GitInput(branch, 256) || branch.starts_with('-') ||
      !GitInput(proposed_directory))
    return Fail<WorktreePreview>(kit::ErrorCode::InvalidArgument, "Enter a Git revision, branch and absolute destination");
  const auto destination = NormalDirectory(fs::u8path(proposed_directory));
  if (!destination) return Fail<WorktreePreview>(kit::ErrorCode::InvalidArgument, "Choose a target in an existing local directory");
  auto root = GitValue(storage, source.root, {"rev-parse", "--show-toplevel"});
  if (!root.Succeeded()) return Result::Failure(root.Error());
  auto repo = Canonical(fs::u8path(root.Value()));
  if (!repo || !Descendant(source.root, *repo) || Descendant(*destination, *repo))
    return Fail<WorktreePreview>(kit::ErrorCode::InvalidArgument, "Choose a worktree directory outside the source repository");
  auto common = GitValue(storage, *repo, {"rev-parse", "--path-format=absolute", "--git-common-dir"});
  if (!common.Succeeded()) return Result::Failure(common.Error());
  auto git_dir = Canonical(fs::u8path(common.Value()));
  if (!git_dir) return Fail<WorktreePreview>(kit::ErrorCode::NotFound, "The Git repository directory is unavailable");
  auto commit = GitValue(storage, *repo, {"rev-parse", "--verify", "--end-of-options", revision + "^{commit}"});
  if (!commit.Succeeded()) return Result::Failure(commit.Error());
  auto branch_name = GitValue(storage, *repo, {"check-ref-format", "--branch", branch});
  if (!branch_name.Succeeded()) return Result::Failure(branch_name.Error());
  if (branch_name.Value() != branch) return Fail<WorktreePreview>(kit::ErrorCode::InvalidArgument, "Enter a literal new branch name");

  auto plan = kit::NewCommandId().value;
  Json record{{"principal", principal.value}, {"boundary", Boundary(boundary)}, {"workspace", source.id.value},
      {"project", Project(project)}, {"source_root", Utf8(source.root)}, {"repo", Utf8(*repo)}, {"common_dir", Utf8(*git_dir)},
      {"commit", std::move(commit).Value()}, {"directory", Utf8(*destination)}, {"branch", std::move(branch)},
      {"title", std::move(title)}, {"command", kit::NewCommandId().value}, {"expires", Now() + (10min / 1ms)}};
  if (comparison) record["comparison"] = comparison->value;
  if (!Publish(storage, storage.Child(plan + ".plan"), record))
    return Fail<WorktreePreview>(kit::ErrorCode::PersistenceFailure, "Could not save the worktree preview");
  return Result::Success(ReadPreview(std::move(plan), record));
}

kit::AgentResult<std::vector<WorktreePreview>> PrepareComparison(const File& storage, CodeWorkspace source,
    kit::PrincipalId principal, kit::CommandId command, const kit::ForkReviewRequest& request) {
  using Result = kit::AgentResult<std::vector<WorktreePreview>>;
  if (!Identifier(command.value)) return Fail<std::vector<WorktreePreview>>(kit::ErrorCode::InvalidArgument, "Invalid comparison command");
  const auto checkouts = storage.Child("candidates");
  if (!checkouts.CreateDirectories())
    return Fail<std::vector<WorktreePreview>>(kit::ErrorCode::PersistenceFailure, "Could not prepare the candidate worktree directory");
  const auto commit = GitValue(storage, source.root, {"rev-parse", "--verify", "HEAD^{commit}"});
  if (!commit.Succeeded()) return Result::Failure(commit.Error());
  std::vector<WorktreePreview> previews;
  for (const auto& candidate : request.candidates) {
    const auto suffix = command.value + "-" + std::to_string(previews.size() + 1);
    auto preview = Prepare(storage, source, principal, request.from, request.project, candidate.session.title,
        commit.Value(), checkouts.Child(suffix).Path(), "agentkit/comparison/" + suffix, command);
    if (!preview.Succeeded()) return Result::Failure(preview.Error());
    previews.push_back(std::move(preview).Value());
  }
  return Result::Success(std::move(previews));
}

kit::AgentResult<File> Allocate(const File& storage, CodeWorkspace source, const kit::WorkspaceRequest& request) {
  using Result = kit::AgentResult<File>;
  const auto& id = request.choice->copy_plan->id.value;
  if (storage.Child(id + ".discarded").Exists() || storage.Child(id + ".discarding").Exists())
    return Fail<File>(kit::ErrorCode::AlreadyClosed, "The workspace copy was abandoned");
  const auto plan = ReadRecord(storage.Child(id + ".plan"));
  if (!plan) return Fail<File>(kit::ErrorCode::NotFound, "The worktree preview is unavailable");
  if (plan->at("principal") != request.actor.principal.value || plan->at("boundary") != Boundary(*request.fork_from) ||
      plan->at("workspace") != source.id.value || plan->at("project") != Project(request.project) ||
      plan->at("source_root") != Utf8(source.root))
    return Fail<File>(kit::ErrorCode::PermissionDenied, "The worktree preview belongs to a different source or request");
  const bool comparison_matches = request.comparison_command
      ? plan->contains("comparison") && plan->at("comparison") == request.comparison_command->value
      : !plan->contains("comparison") && plan->at("command") == request.command.value;
  if (!comparison_matches)
    return Fail<File>(kit::ErrorCode::Conflict, "Use the original command for this worktree preview");
  const auto owner_file = storage.Child(id + ".owner");
  const Json owner{{"command", request.command.value}, {"session", request.new_session.value}};
  auto prior = ReadRecord(owner_file);
  if (prior && *prior != owner) return Fail<File>(kit::ErrorCode::Conflict, "Another command has consumed the worktree preview");
  if (!prior && Now() >= plan->at("expires").get<std::int64_t>())
    return Fail<File>(kit::ErrorCode::DeadlineExceeded, "The worktree preview expired. Prepare a new one");

  const auto repo = fs::u8path(plan->at("repo").get<std::string>());
  const auto destination = fs::u8path(plan->at("directory").get<std::string>());
  const auto branch = plan->at("branch").get<std::string>();
  const auto commit = plan->at("commit").get<std::string>();
  const auto lock = "AgentKit:" + id;
  auto current = GitValue(storage, source.root, {"rev-parse", "--path-format=absolute", "--git-common-dir"});
  if (!current.Succeeded()) return Result::Failure(current.Error());
  const auto common = Canonical(fs::u8path(current.Value()));
  if (!common || Utf8(*common) != plan->at("common_dir").get<std::string>() ||
      NormalDirectory(destination) != destination || Descendant(destination, repo))
    return Fail<File>(kit::ErrorCode::Conflict, "The repository identity or target directory changed");
  auto checkout = Inspect(storage, repo, destination);
  if (!checkout.Succeeded()) return Result::Failure(checkout.Error());
  if (!checkout.Value()) {
    if (Occupied(destination)) return Fail<File>(kit::ErrorCode::Conflict, "The worktree destination is already occupied");
    auto existing_branch = RunGit(storage, repo, {"show-ref", "--verify", "--quiet", "refs/heads/" + branch});
    if (!existing_branch.Succeeded()) return Result::Failure(existing_branch.Error());
    if (existing_branch.Value().exit_code == 0) return Fail<File>(kit::ErrorCode::Conflict, "The target branch already exists");
    if (existing_branch.Value().exit_code != 1) return Fail<File>(kit::ErrorCode::Conflict, "Could not check the target branch");
    auto pinned = GitValue(storage, repo, {"rev-parse", "--verify", "--end-of-options", commit + "^{commit}"});
    if (!pinned.Succeeded()) return Result::Failure(pinned.Error());
    if (pinned.Value() != commit) return Fail<File>(kit::ErrorCode::Conflict, "The approved Git commit changed");
    if (!prior && !Publish(storage, owner_file, owner))
      return Fail<File>(kit::ErrorCode::Conflict, "Could not claim the worktree preview");
    auto added = RunGit(storage, repo, {"worktree", "add", "--lock", "--reason", lock, "-b", branch, "--", Utf8(destination), commit});
    if (!added.Succeeded()) return Result::Failure(added.Error());
    if (added.Value().exit_code != 0) return Fail<File>(kit::ErrorCode::Conflict, Unline(std::move(added.Value().output)));
    checkout = Inspect(storage, repo, destination);
    if (!checkout.Succeeded()) return Result::Failure(checkout.Error());
  } else if (!prior) {
    return Fail<File>(kit::ErrorCode::Conflict, "The target belongs to an existing worktree");
  }
  if (!checkout.Value() || checkout.Value()->head != commit || checkout.Value()->branch != "refs/heads/" + branch ||
      checkout.Value()->lock != lock || Canonical(destination) != destination)
    return Fail<File>(kit::ErrorCode::ResultUnknown, "Could not confirm the worktree's commit, branch and ownership");

  auto clean = GitValue(storage, destination, {"status", "--porcelain", "-z", "--untracked-files=all"});
  if (!clean.Succeeded()) return Result::Failure(clean.Error());
  if (!clean.Value().empty()) return Fail<File>(kit::ErrorCode::Conflict, "The allocated worktree contains changes");
  const auto scope = source.root == repo ? destination : destination / source.root.lexically_relative(repo);
  if (!Descendant(scope, destination) || Canonical(scope) != scope)
    return Fail<File>(kit::ErrorCode::NotFound, "The selected workspace folder is unavailable at the approved commit");
  return Result::Success(File(scope.u8string()));
}

kit::AgentResult<void> Confirm(const File& storage, kit::WorkspaceCopyPlanRef ref,
    const kit::CommandId& command, const kit::SessionId& session) {
  const auto& id = ref.id.value;
  if (storage.Child(id + ".discarded").Exists() || storage.Child(id + ".discarding").Exists())
    return Fail<void>(kit::ErrorCode::AlreadyClosed, "The workspace copy was abandoned");
  const auto owner = ReadRecord(storage.Child(id + ".owner"));
  const auto plan = ReadRecord(storage.Child(id + ".plan"));
  if (!owner || !plan || owner->at("command") != command.value || owner->at("session") != session.value)
    return Fail<void>(kit::ErrorCode::Conflict, "The allocated worktree does not belong to this session");
  const auto repo = fs::u8path(plan->at("repo").get<std::string>());
  const auto destination = fs::u8path(plan->at("directory").get<std::string>());
  auto checkout = Inspect(storage, repo, destination);
  if (!checkout.Succeeded()) return kit::AgentResult<void>::Failure(checkout.Error());
  if (!checkout.Value()) return Fail<void>(kit::ErrorCode::NotFound, "The allocated worktree is unavailable");
  if (!checkout.Value()->lock.empty()) {
    if (checkout.Value()->lock != "AgentKit:" + id)
      return Fail<void>(kit::ErrorCode::Conflict, "The worktree now has a different lock owner");
    auto released = GitValue(storage, repo, {"worktree", "unlock", "--", Utf8(destination)});
    if (!released.Succeeded()) return kit::AgentResult<void>::Failure(released.Error());
  }
  const auto settled = storage.Child(id + ".settled");
  if (!settled.Exists() && !Publish(storage, settled, *owner))
    return Fail<void>(kit::ErrorCode::PersistenceFailure, "Could not record the finished worktree copy");
  return kit::AgentResult<void>::Success();
}

struct DiscardPlan {
  WorktreePreview preview;
  Json owner;
  fs::path repo;
  fs::path common_directory;
  fs::path destination;
  fs::path scope;
};

kit::AgentResult<DiscardPlan> ReadDiscardPlan(const File& storage, const kit::WorkspaceCopyPlanRef& ref,
    const kit::CommandId& command, const kit::PrincipalId& principal) {
  using Result = kit::AgentResult<DiscardPlan>;
  const auto& id = ref.id.value;
  const auto plan = ReadRecord(storage.Child(id + ".plan"));
  const auto owner = ReadRecord(storage.Child(id + ".owner"));
  if (!plan || !owner || plan->at("principal") != principal.value ||
      owner->at("session") != "workspace-fork:" + id + ":1")
    return Fail<DiscardPlan>(kit::ErrorCode::PermissionDenied, "The worktree ownership record could not be verified");
  if (storage.Child(id + ".settled").Exists())
    return Fail<DiscardPlan>(kit::ErrorCode::Conflict, "The worktree already belongs to a committed conversation");
  auto preview = ReadPreview(id, *plan);
  if (preview.comparison ? *preview.comparison != command : preview.command != command || owner->at("command") != command.value)
    return Fail<DiscardPlan>(kit::ErrorCode::PermissionDenied, "The worktree belongs to a different command");
  auto repo = fs::u8path(preview.source);
  auto destination = fs::u8path(preview.directory);
  auto source = fs::u8path(plan->at("source_root").get<std::string>());
  if (preview.comparison) {
    const auto location = Canonical(fs::u8path(storage.Child("candidates").Path()));
    const auto name = Utf8(destination.filename());
    if (!location || destination.parent_path() != *location || !Identifier(name) ||
        !name.starts_with(command.value + "-") || preview.branch != "agentkit/comparison/" + name)
      return Fail<DiscardPlan>(kit::ErrorCode::Conflict, "The checkout is outside this comparison's managed directory");
  }
  if (!Descendant(source, repo) || !repo.is_absolute() || NormalDirectory(destination) != destination ||
      Descendant(destination, repo) || Descendant(repo, destination))
    return Fail<DiscardPlan>(kit::ErrorCode::Conflict, "The recorded checkout destination is no longer safe to remove");
  auto scope = source == repo ? destination : destination / source.lexically_relative(repo);
  if (!Descendant(scope, destination))
    return Fail<DiscardPlan>(kit::ErrorCode::Conflict, "The recorded workspace is outside the checkout");
  return Result::Success({std::move(preview), *owner, std::move(repo), fs::u8path(plan->at("common_dir").get<std::string>()),
                          std::move(destination), std::move(scope)});
}

struct DiscardCheckout {
  fs::path location;
  fs::path administrative;
  std::string lock;
};

fs::path CleanupDirectory(const DiscardPlan& plan) {
  return plan.destination.parent_path() / (".agentkit-cleanup-" + plan.preview.plan.id.value);
}

kit::AgentResult<std::optional<DiscardCheckout>> CheckDiscard(const File& storage, const DiscardPlan& plan) {
  using Result = kit::AgentResult<std::optional<DiscardCheckout>>;
  const auto& id = plan.preview.plan.id.value;
  const auto quarantine = CleanupDirectory(plan);
  auto recorded = GitValue(storage, plan.repo, {"rev-parse", "--path-format=absolute", "--git-common-dir"});
  if (!recorded.Succeeded()) return Result::Failure(recorded.Error());
  if (Canonical(fs::u8path(recorded.Value())) != plan.common_directory ||
      NormalDirectory(quarantine) != quarantine)
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The Git repository or target directory changed");
  auto location = plan.destination;
  auto checkout = Inspect(storage, plan.repo, location);
  if (!checkout.Succeeded()) return Result::Failure(checkout.Error());
  const auto started = ReadRecord(storage.Child(id + ".discarding"));
  if (started && started->at("owner") != plan.owner)
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The cleanup record has a different owner");
  if (checkout.Value() && Occupied(quarantine))
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The cleanup directory is already occupied");
  if (!checkout.Value() && started) {
    location = quarantine;
    checkout = Inspect(storage, plan.repo, location);
    if (!checkout.Succeeded()) return Result::Failure(checkout.Error());
    if (!checkout.Value() && !Occupied(quarantine) && !Occupied(plan.destination))
      return Result::Success(std::nullopt);
  }
  if (!checkout.Value()) {
    if (Occupied(plan.destination) || Occupied(quarantine))
      return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The recorded directory now belongs to another operation");
    if (plan.preview.comparison) {
      auto branch = RunGit(storage, plan.repo, {"show-ref", "--verify", "--quiet", "refs/heads/" + plan.preview.branch});
      if (!branch.Succeeded()) return Result::Failure(branch.Error());
      if (branch.Value().exit_code == 1) return Result::Success(std::nullopt);
    }
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::ResultUnknown, "The checkout ownership could not be verified");
  }
  if (checkout.Value()->head != plan.preview.commit || checkout.Value()->branch != "refs/heads/" + plan.preview.branch ||
      Canonical(location) != location)
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The checkout's path, branch or commit changed");
  auto administrative = GitValue(storage, location, {"rev-parse", "--absolute-git-dir"});
  if (!administrative.Succeeded()) return Result::Failure(administrative.Error());
  auto directory = Canonical(fs::u8path(administrative.Value()));
  if (!directory || directory->parent_path() != plan.common_directory / "worktrees")
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The checkout does not belong to the recorded repository");
  const Json marker{{"owner", plan.owner}, {"git_dir", Utf8(*directory)}};
  if (started && *started != marker)
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The checkout was replaced during cleanup");
  if (checkout.Value()->lock != "AgentKit:" + id &&
      (!checkout.Value()->lock.empty() || !started ||
       ReadRecord(File(directory->u8string()).Child("agentkit-discard")) != marker))
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, "The checkout no longer belongs to this copy");
  auto clean = GitValue(storage, location,
      {"status", "--porcelain", "-z", "--untracked-files=all", "--ignored=matching", "--ignore-submodules=none"});
  if (!clean.Succeeded()) return Result::Failure(clean.Error());
  if (!clean.Value().empty()) {
    auto message = std::string("The checkout contains changes or extra files and was kept");
    if (location != plan.destination) message += " at " + Utf8(location);
    return Fail<std::optional<DiscardCheckout>>(kit::ErrorCode::Conflict, std::move(message));
  }
  return Result::Success(DiscardCheckout{std::move(location), std::move(*directory), std::move(checkout.Value()->lock)});
}

kit::AgentResult<void> RemoveCheckout(const File& storage, const DiscardPlan& plan) {
  const auto& id = plan.preview.plan.id.value;
  auto checked = CheckDiscard(storage, plan);
  if (!checked.Succeeded()) return kit::AgentResult<void>::Failure(checked.Error());
  if (!checked.Value()) return kit::AgentResult<void>::Success();
  const auto checkout = *checked.Value();
  const Json marker{{"owner", plan.owner}, {"git_dir", Utf8(checkout.administrative)}};
  const auto started = storage.Child(id + ".discarding");
  const auto witness = File(checkout.administrative.u8string()).Child("agentkit-discard");
  if ((!Publish(storage, started, marker) && ReadRecord(started) != marker) ||
      (!Publish(File(checkout.administrative.u8string()), witness, marker) && ReadRecord(witness) != marker))
    return Fail<void>(kit::ErrorCode::PersistenceFailure, "Could not record ownership for recoverable cleanup");
  if (checkout.lock == "AgentKit:" + id) {
    auto unlocked = GitValue(storage, plan.repo, {"worktree", "unlock", "--", Utf8(checkout.location)});
    if (!unlocked.Succeeded()) return kit::AgentResult<void>::Failure(unlocked.Error());
  }
  checked = CheckDiscard(storage, plan);
  if (!checked.Succeeded()) return kit::AgentResult<void>::Failure(checked.Error());
  if (!checked.Value()) return kit::AgentResult<void>::Success();
  const auto quarantine = CleanupDirectory(plan);
  if (checked.Value()->location == plan.destination) {
    // Moving the checkout first isolates it from writers that still use the original path.
    auto moved = GitValue(storage, plan.repo, {"worktree", "move", "--", Utf8(plan.destination), Utf8(quarantine)});
    if (!moved.Succeeded()) return kit::AgentResult<void>::Failure(moved.Error());
    checked = CheckDiscard(storage, plan);
    if (!checked.Succeeded()) return kit::AgentResult<void>::Failure(checked.Error());
    if (!checked.Value()) return kit::AgentResult<void>::Success();
  }
  if (checked.Value()->location != quarantine)
    return Fail<void>(kit::ErrorCode::Conflict, "The checkout was not isolated for removal");
  auto removed = GitValue(storage, plan.repo, {"worktree", "remove", "--", Utf8(quarantine)});
  if (!removed.Succeeded()) return kit::AgentResult<void>::Failure(removed.Error());
  checked = CheckDiscard(storage, plan);
  if (!checked.Succeeded()) return kit::AgentResult<void>::Failure(checked.Error());
  if (checked.Value()) return Fail<void>(kit::ErrorCode::ResultUnknown, "Git did not confirm checkout removal");
  return kit::AgentResult<void>::Success();
}

kit::AgentResult<void> FinishDiscard(const File& storage, const DiscardPlan& plan) {
  const auto record = storage.Child(plan.preview.plan.id.value + ".discarded");
  if (!Publish(storage, record, plan.owner) && ReadRecord(record) != plan.owner)
    return Fail<void>(kit::ErrorCode::PersistenceFailure, "Could not record the completed cleanup");
  return kit::AgentResult<void>::Success();
}

} // namespace

WorktreeForks::WorktreeForks(File directory, std::shared_ptr<CodeWorkspaces> workspaces)
    : directory_(std::move(directory)), workspaces_(std::move(workspaces)) {}

kit::Async<void> WorktreeForks::LoadAsync() {
  if (!co_await directory_.CreateDirectoriesAsync())
    co_return Fail<void>(kit::ErrorCode::PersistenceFailure, "Could not open the worktree plan registry");
  co_return kit::AgentResult<void>::Success();
}

kit::Async<WorktreePreview> WorktreeForks::PrepareAsync(kit::PrincipalId principal, kit::HistoryBoundary boundary,
    kit::WorkspaceId workspace, std::optional<kit::ProjectId> project, std::string title, std::string revision,
    std::string directory, std::string branch) {
  auto source = workspaces_->Find(workspace);
  if (!source) co_return Fail<WorktreePreview>(kit::ErrorCode::NotFound, "The source workspace is unavailable");
  try {
    co_return co_await huxerui::RunWorker(Prepare, directory_, std::move(*source), std::move(principal),
        std::move(boundary), std::move(project), std::move(title), std::move(revision), std::move(directory),
        std::move(branch), std::optional<kit::CommandId>{});
  } catch (const std::exception& error) {
    co_return Fail<WorktreePreview>(kit::ErrorCode::InvalidArgument, error.what());
  }
}

kit::Async<std::vector<WorktreePreview>> WorktreeForks::PrepareComparisonAsync(kit::PrincipalId principal,
    kit::WorkspaceId workspace, kit::CommandId command, kit::ForkReviewRequest request) {
  auto source = workspaces_->Find(workspace);
  if (!source)
    co_return Fail<std::vector<WorktreePreview>>(kit::ErrorCode::NotFound, "The source workspace is unavailable");
  try {
    co_return co_await huxerui::RunWorker(PrepareComparison, directory_, std::move(*source), std::move(principal),
        std::move(command), std::move(request));
  } catch (const std::exception& error) {
    co_return Fail<std::vector<WorktreePreview>>(kit::ErrorCode::PersistenceFailure, error.what());
  }
}

kit::Async<std::vector<WorktreePreview>> WorktreeForks::PendingAsync(
    std::shared_ptr<kit::AgentRuntime> runtime, kit::PrincipalId principal) {
  using Result = kit::AgentResult<std::vector<WorktreePreview>>;
#ifdef __EMSCRIPTEN__
  co_return Result::Success({});
#else
  try {
    auto candidates = co_await huxerui::RunWorker(Pending, directory_, std::move(principal));
    std::vector<WorktreePreview> result;
    for (auto& copy : candidates) {
      if (copy.comparison) {
        auto progress = co_await runtime->Commands().ReadAsync(*copy.comparison);
        if (!progress.Succeeded() || progress.Value().accepted.session != copy.request.from.session ||
            progress.Value().state == kit::CommandState::Applied || progress.Value().state == kit::CommandState::Scheduled ||
            progress.Value().state == kit::CommandState::AwaitingApproval) continue;
        copy.recovery_error = progress.Value().failure;
      }
      result.push_back(std::move(copy));
    }
    co_return Result::Success(std::move(result));
  } catch (const std::exception& error) {
    co_return Fail<std::vector<WorktreePreview>>(kit::ErrorCode::PersistenceFailure, error.what());
  }
#endif
}

kit::Async<std::optional<kit::WorkspaceId>> WorktreeForks::AllocateAsync(kit::WorkspaceRequest request) {
  using Result = kit::AgentResult<std::optional<kit::WorkspaceId>>;
  const auto& choice = request.choice;
  if (!choice || choice->kind != kit::WorkspaceChoice::Kind::CopyFromSource || !choice->copy_plan || !request.fork_from ||
      !Identifier(choice->copy_plan->id.value) || choice->copy_plan->version != 1)
    co_return Fail<std::optional<kit::WorkspaceId>>(kit::ErrorCode::InvalidArgument, "The worktree preview is invalid");
  auto source = workspaces_->Find(choice->workspace);
  if (!source) co_return Fail<std::optional<kit::WorkspaceId>>(kit::ErrorCode::PermissionDenied, "The source workspace is unavailable");
  try {
    auto lock = co_await huxerui::RunWorker(LockPlan, directory_, choice->copy_plan->id);
    if (!lock.Succeeded()) co_return Result::Failure(lock.Error());
    auto allocated = co_await huxerui::RunWorker(Allocate, directory_, std::move(*source), request);
    if (!allocated.Succeeded()) co_return Result::Failure(allocated.Error());
    kit::WorkspaceId id{choice->copy_plan->id.value};
    auto saved = co_await workspaces_->RegisterForkAsync(id, std::move(allocated).Value(), request.project);
    if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
    co_return Result::Success(std::move(id));
  } catch (const std::exception& error) {
    co_return Fail<std::optional<kit::WorkspaceId>>(kit::ErrorCode::PersistenceFailure, error.what());
  }
}

kit::Async<void> WorktreeForks::ConfirmAsync(kit::WorkspaceCopyPlanRef plan, kit::CommandId command, kit::SessionId session) {
  if (!Identifier(plan.id.value) || plan.version != 1)
    co_return Fail<void>(kit::ErrorCode::InvalidArgument, "The worktree preview is invalid");
  try {
    auto lock = co_await huxerui::RunWorker(LockPlan, directory_, plan.id);
    if (!lock.Succeeded()) co_return kit::AgentResult<void>::Failure(lock.Error());
    co_return co_await huxerui::RunWorker(Confirm, directory_, std::move(plan), std::move(command), std::move(session));
  } catch (const std::exception& error) {
    co_return Fail<void>(kit::ErrorCode::PersistenceFailure, error.what());
  }
}

kit::Async<void> WorktreeForks::DiscardAsync(std::shared_ptr<kit::AgentRuntime> runtime, kit::PrincipalId principal,
    kit::WorkspaceCopyPlanRef ref, kit::CommandId command) {
  using Result = kit::AgentResult<void>;
  if (!Identifier(ref.id.value) || ref.version != 1)
    co_return Fail<void>(kit::ErrorCode::InvalidArgument, "The worktree preview is invalid");
  try {
    auto lock = co_await huxerui::RunWorker(LockPlan, directory_, ref.id);
    if (!lock.Succeeded()) co_return Result::Failure(lock.Error());
    auto journal = co_await huxerui::RunWorker(ReadDiscardPlan, directory_, ref, std::move(command), std::move(principal));
    if (!journal.Succeeded()) co_return Result::Failure(journal.Error());
    const auto& plan = journal.Value();
    auto checked = co_await huxerui::RunWorker(CheckDiscard, directory_, plan);
    if (!checked.Succeeded()) co_return Result::Failure(checked.Error());
    const kit::WorkspaceId workspace{ref.id.value};
    auto registry = co_await workspaces_->CheckForkRegistrationAsync(
        workspace, plan.destination, plan.scope, plan.preview.request.options.project);
    if (!registry.Succeeded()) co_return registry;
    auto retire = plan.preview.comparison
        ? runtime->ForkReviews().AbandonComparisonWorkspaceAsync(*plan.preview.comparison, ref, workspace)
        : runtime->Sessions().AbandonWorkspaceForkAsync(plan.preview.command, plan.preview.request, workspace);
    auto abandoned = co_await std::move(retire);
    if (!abandoned.Succeeded()) co_return abandoned;
    auto removed = co_await huxerui::RunWorker(RemoveCheckout, directory_, plan);
    if (!removed.Succeeded()) co_return removed;
    registry = co_await workspaces_->RemoveForkRegistrationAsync(
        workspace, plan.destination, plan.scope, plan.preview.request.options.project);
    if (!registry.Succeeded()) co_return registry;
    co_return co_await huxerui::RunWorker(FinishDiscard, directory_, plan);
  } catch (const std::exception& error) {
    co_return Fail<void>(kit::ErrorCode::PersistenceFailure, error.what());
  }
}

} // namespace agentkit_preview
