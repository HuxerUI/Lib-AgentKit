#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

bool Active(CandidateState state) {
  return state == CandidateState::Queued || state == CandidateState::Running;
}

bool ActiveAdoption(ToolCallState state) {
  return state == ToolCallState::Preparing || state == ToolCallState::Reviewing ||
      state == ToolCallState::AwaitingApproval || state == ToolCallState::Ready || state == ToolCallState::Executing;
}

Budget CandidateBudget(const Budget& total, std::size_t index, std::size_t count) {
  const auto share = [index, count](std::optional<std::uint64_t> cap) -> std::optional<std::uint64_t> {
    if (!cap) return {};
    return *cap / count + (index < *cap % count ? 1 : 0);
  };
  return {share(total.tokens), share(total.model_turns), share(total.tool_calls), share(total.context_bytes), total.deadline};
}

AgentResult<void> CheckComparison(const ForkReviewRequest& input, bool durable) {
  if (input.candidates.size() < 2 ||
      (input.shared_budget.deadline && *input.shared_budget.deadline <= Duration::zero())) {
    return Fail<void>(ErrorCode::InvalidArgument, "Choose at least two candidates and a positive comparison deadline");
  }
  std::set<WorkspaceCopyPlanId> copies;
  for (const auto& candidate : input.candidates) {
    if (candidate.session.workspace || (candidate.session.project && candidate.session.project != input.project) ||
        (candidate.session.retention == SessionRetention::Stored) != durable ||
        (!candidate.task.context.empty() && candidate.task.context != input.inputs) ||
        !candidate.task.side_chat_conclusions.empty() || !candidate.task.recovered_conclusions.empty()) {
      return Fail<void>(ErrorCode::InvalidArgument, "Candidates must use the comparison's project, retention and source versions");
    }
    const auto& workspace = candidate.workspace;
    if (workspace.kind == WorkspaceChoice::Kind::CopyFromSource) {
      if (workspace.workspace.value.empty() || !workspace.copy_plan || workspace.copy_plan->id.value.empty() ||
          !workspace.copy_plan->version || !copies.insert(workspace.copy_plan->id).second) {
        return Fail<void>(ErrorCode::InvalidArgument, "Each isolated candidate needs a distinct workspace copy plan");
      }
    } else if (workspace.kind != WorkspaceChoice::Kind::Existing || workspace.copy_plan) {
      return Fail<void>(ErrorCode::InvalidArgument, "Invalid candidate workspace choice");
    }
  }
  return ValidateJson(input.comparison_criteria, JsonSchema{JsonValue{true}});
}

PolicyRequest ReviewPolicy(RuntimeState& runtime, const ActionParty& actor, const ForkReviewSnapshot& review, Capability capability) {
  auto policy = runtime.ProjectPolicy(actor, review.request.project, capability);
  policy.target.review = review.id;
  policy.target.session = review.request.from.session;
  policy.sources = review.request.inputs;
  return policy;
}

ApprovalPreview ComparisonPreview(const ForkReviewRequest& request, const std::vector<PreparedForkCandidate>& candidates) {
  ApprovalPreview preview{.title = "Approve comparison"};
  if (const auto* criteria = std::get_if<std::string>(&request.comparison_criteria.value))
    preview.summary = *criteria + "\n\n";
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto& planned = candidates[i];
    const auto& candidate = request.candidates[i];
    if (i) preview.summary += "\n\n";
    preview.summary += planned.session.summary.title + " (" + planned.session.summary.agent.id.value + ")\n" + candidate.task.text;
    if (candidate.workspace.copy_plan)
      preview.summary += "\nWorkspace copy plan: " + candidate.workspace.copy_plan->id.value;
  }
  for (const auto& source : request.inputs) {
    if (const auto* ref = std::get_if<ArtifactRef>(&source.source);
        ref && std::ranges::find(preview.artifacts, *ref) == preview.artifacts.end()) preview.artifacts.push_back(*ref);
  }
  return preview;
}

} // namespace

std::optional<ProjectId> RuntimeState::FindForkReviewProject(const ForkReviewId& review) const {
  for (const auto& [project_id, data] : projects) {
    const auto* image = PublishedProject(project_id);
    if (image && image->fork_reviews.contains(review)) return project_id;
  }
  return {};
}

Async<void> RuntimeState::FilterComparisonResult(ForkReviewCreated& result, std::optional<RunId> bound) {
  auto review = co_await ReadForkReview(result.review, bound);
  if (!review.Succeeded()) co_return AgentResult<void>::Failure(review.Error());
  std::erase_if(result.branches, [&](const auto& branch) {
    return std::ranges::find(review.Value().candidates, branch, &ForkCandidate::session) == review.Value().candidates.end();
  });
  co_return AgentResult<void>::Success();
}

Async<Receipt<ForkReviewCreated>> RuntimeState::ReadComparisonCommand(const CommandId& command, std::optional<RunId> bound) {
  using Result = AgentResult<Receipt<ForkReviewCreated>>;
  auto current = co_await ReadCommand(command, bound);
  if (!current.Succeeded()) {
    if (storage_error && current.Error().code == ErrorCode::NotFound) co_return Result::Failure(*storage_error);
    if (current.Error().code == ErrorCode::NotFound)
      co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Busy, "The comparison command is being saved");
    co_return Result::Failure(current.Error());
  }
  auto progress = std::move(current).Value();
  if (progress.state == CommandState::Applied && progress.result) {
    if (auto* result = std::get_if<ForkReviewCreated>(&*progress.result); result && progress.resolution)
      co_return Result::Success({std::move(*progress.resolution), std::move(*result)});
  }
  if (progress.failure) co_return Result::Failure(std::move(*progress.failure));
  if (storage_error) co_return Result::Failure(*storage_error);
  co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Busy, "The comparison command is still being processed");
}

Async<Receipt<ForkReviewCreated>> RuntimeState::CreateForkReview(CommandId command, ForkReviewRequest request,
    std::optional<RunId> bound, std::optional<OperationPlan> approved) {
  using Result = AgentResult<Receipt<ForkReviewCreated>>;
  auto actor = co_await ProjectActor(request.project, bound, Capability::CreateComparison);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  std::vector<Capability> confirmation;
  auto allowed = co_await CheckOperation(ProjectPolicy(actor.Value(), request.project, Capability::CreateComparison),
      confirmation, approved);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto checked = CheckComparison(request, !!options.store);
  if (!checked.Succeeded()) co_return Result::Failure(checked.Error());
  const auto source = sessions.find(request.from.session);
  const auto* image = source == sessions.end() ? nullptr : Published(source->second);
  if (!image || image->summary.project != request.project || image->summary.lifecycle == SessionLifecycle::Deleting ||
      (options.store && image->retention != SessionRetention::Stored)) {
    co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::NotFound, "Comparison history is unavailable in this project");
  }
  const auto binding = image->summary.binding_revision;
  const auto access = access_revision;
  const auto source_current = [&] {
    const auto found = sessions.find(request.from.session);
    const auto* current = found == sessions.end() ? nullptr : Published(found->second);
    return current && current->summary.binding_revision == binding && current->summary.project == request.project &&
        current->summary.lifecycle != SessionLifecycle::Deleting && access == access_revision;
  };
  if (approved && (approved->target_binding != binding || !approved->comparison ||
      approved->comparison->candidates.size() != request.candidates.size()))
    co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Conflict, "The approved comparison source or candidates changed");
  allowed = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, source->second));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  ProjectCommand input{request};
  const CommandInput key{.operation = Operation::CreateComparison, .session = request.from.session,
      .run = bound.value_or(RunId{}), .comparison = request};
  auto replay_command = [&]() -> Async<std::optional<Receipt<ForkReviewCreated>>> {
    using Replay = AgentResult<std::optional<Receipt<ForkReviewCreated>>>;
    if (commands.contains({actor.Value().principal, command})) {
      auto staged = Find(actor.Value().principal, command, key);
      if (!staged.Succeeded()) co_return Replay::Failure(staged.Error());
      if (approved) {
        if (staged.Value()->progress.state != CommandState::Scheduled)
          co_return Fail<std::optional<Receipt<ForkReviewCreated>>>(ErrorCode::AlreadyClosed, "The comparison approval has ended");
        co_return Replay::Success({});
      }
      auto read = staged.Value()->progress.approval
          ? ReplayReceipt(ReceiptFor<ForkReviewCreated>(actor.Value().principal, command))
          : ReadComparisonCommand(command, bound);
      auto receipt = co_await std::move(read);
      if (!receipt.Succeeded()) co_return Replay::Failure(receipt.Error());
      co_return Replay::Success(std::move(receipt).Value());
    }
    if (approved) co_return Fail<std::optional<Receipt<ForkReviewCreated>>>(ErrorCode::NotFound, "Comparison approval unavailable");
    auto previous = ReplayProject<ForkReviewCreated>(actor.Value(), request.project, command, input);
    if (!previous.Succeeded()) co_return Replay::Failure(previous.Error());
    if (!previous.Value()) co_return Replay::Success({});
    auto receipt = co_await ReplayReceipt(Result::Success(*previous.Value()));
    if (!receipt.Succeeded()) co_return Replay::Failure(receipt.Error());
    auto visible = co_await FilterComparisonResult(std::get<ForkReviewCreated>(receipt.Value().outcome), bound);
    if (!visible.Succeeded()) co_return Replay::Failure(visible.Error());
    co_return Replay::Success(std::move(receipt).Value());
  };
  auto replay = co_await replay_command();
  if (!replay.Succeeded()) co_return Result::Failure(replay.Error());
  if (replay.Value()) co_return Result::Success(std::move(*replay.Value()));
  if (storage_error && options.store) co_return Result::Failure(*storage_error);
  const auto [pending, inserted] = preparing_fork_reviews.emplace(std::pair{actor.Value().principal, command}, request);
  if (!inserted) co_return Fail<Receipt<ForkReviewCreated>>(pending->second == request ? ErrorCode::Busy : ErrorCode::Conflict,
      "This comparison command is already in preparation");
  auto release = std::unique_ptr<const std::pair<PrincipalId, CommandId>,
      std::function<void(const std::pair<PrincipalId, CommandId>*)>>(&pending->first,
          [this](const auto* key) { preparing_fork_reviews.erase(*key); });
  if (bound) {
    const auto* scope = Scope(runs.at(*bound));
    if (scope && std::ranges::any_of(request.inputs, [&](const auto& source) {
          return std::ranges::find(scope->inputs, source) == scope->inputs.end();
        })) co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::PermissionDenied, "Comparison inputs exceed the acting run's scope");
  }
  ForkReviewSnapshot review{.id = approved ? approved->comparison->review : ForkReviewId{MakeId()}, .revision = 1,
      .state = ForkReviewState::Running, .request = request};
  auto policy = ReviewPolicy(*this, actor.Value(), review, Capability::CreateComparison);
  policy.operation = request.comparison_criteria;
  allowed = co_await CheckOperation(std::move(policy), confirmation, approved);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await ValidateProjectSources(request.project, actor.Value(), request.inputs);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());

  std::vector<PreparedForkCandidate> prepared;
  const auto goal = ActorGoal(actor.Value());
  auto prepare = [&](std::size_t i, bool inspect, const CandidateApprovalBinding* pinned) -> Async<PreparedForkCandidate> {
    using Prepared = AgentResult<PreparedForkCandidate>;
    const auto& candidate = request.candidates[i];
    auto options_for_candidate = candidate.session;
    options_for_candidate.project = request.project;
    std::optional<WorkspaceChoice> copy;
    if (candidate.workspace.kind == WorkspaceChoice::Kind::CopyFromSource) copy = candidate.workspace;
    else if (!candidate.workspace.workspace.value.empty()) options_for_candidate.workspace = candidate.workspace.workspace;
    auto initial = candidate.task;
    initial.context = request.inputs;
    CreateSessionRequest create{.options = std::move(options_for_candidate), .initial = std::move(initial)};
    PreparedForkCandidate plan{.comparison = command, .inspect = inspect};
    std::optional<OperationPlan> approval;
    if (pinned) {
      plan.reserved_session = pinned->session;
      approval = OperationPlan{.confirmation = pinned->confirmation, .target_binding = binding, .new_agent = pinned->agent,
          .run_start = pinned->start};
    }
    const CommandId child{"agentkit.comparison/" + std::to_string(command.value.size()) + "/" + command.value + "/" + std::to_string(i)};
    auto planned = co_await Create(shared_from_this(), child, std::move(create), bound, request.from, std::move(approval), copy, &plan);
    if (!planned.Succeeded()) co_return Prepared::Failure(planned.Error());
    if (plan.session.summary.id.value.empty())
      co_return Fail<PreparedForkCandidate>(ErrorCode::Conflict, "The candidate command is already assigned");
    if (pinned && plan.start != pinned->start)
      co_return Fail<PreparedForkCandidate>(ErrorCode::Conflict, "The approved candidate model or execution profile changed");
    co_return Prepared::Success(std::move(plan));
  };
  ComparisonApprovalBinding reviewed{.review = review.id};
  for (std::size_t i = 0; i < request.candidates.size(); ++i) {
    auto candidate = co_await prepare(i, true, approved ? &approved->comparison->candidates[i] : nullptr);
    if (!candidate.Succeeded()) co_return Result::Failure(candidate.Error());
    auto plan = std::move(candidate).Value();
    reviewed.candidates.push_back({plan.session.summary.id, plan.session.summary.agent, plan.start, plan.confirmation});
    prepared.push_back(std::move(plan));
  }
  if (!source_current())
    co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Conflict, "Comparison history or authorization changed during review");
  if (!approved && (!confirmation.empty() || std::ranges::any_of(prepared, [](const auto& plan) { return !plan.confirmation.empty(); }))) {
    auto staged = co_await StageOperation(actor.Value().principal, command, key, request.from.session,
        {.confirmation = std::move(confirmation), .target_binding = binding, .comparison = std::move(reviewed)},
        ComparisonPreview(request, prepared), {});
    if (!staged.Succeeded()) co_return Result::Failure(staged.Error());
    co_return co_await ReplayReceipt(ReceiptFor<ForkReviewCreated>(actor.Value().principal, command));
  }
  replay = co_await replay_command();
  if (!replay.Succeeded()) co_return Result::Failure(replay.Error());
  if (replay.Value()) co_return Result::Success(std::move(*replay.Value()));
  if (!approved && options.store && std::ranges::any_of(request.candidates, [](const auto& candidate) {
        return candidate.workspace.kind == WorkspaceChoice::Kind::CopyFromSource;
      })) {
    const auto user = actor.Value().principal;
    auto& home = sessions.at(request.from.session);
    ++home.sequence;
    Accept(user, command, key, home, std::monostate{}, CommandState::Scheduled);
    auto& record = commands.at({user, command});
    record.progress.result.reset();
    record.plan = OperationPlan{.target_binding = binding, .comparison = std::move(reviewed)};
    auto journaled = co_await WhenSaved(Persist(home, false, record.progress.accepted.commit));
    if (!journaled.Succeeded()) co_return Result::Failure(journaled.Error());
    release.reset();
    auto start = [self = shared_from_this(), user, command](CallbackCompletion<void> ready) {
      self->ScheduleOperation(user, command, std::move(ready));
      return [] {};
    };
    co_await AwaitCallback<void>(std::move(start));
    co_return co_await ReadComparisonCommand(command, bound);
  }
  for (std::size_t i = 0; i < prepared.size(); ++i) {
    if (request.candidates[i].workspace.kind == WorkspaceChoice::Kind::CopyFromSource) {
      const auto& pinned = approved ? approved->comparison->candidates[i] : reviewed.candidates[i];
      auto allocated = co_await prepare(i, false, &pinned);
      if (!allocated.Succeeded()) co_return Result::Failure(allocated.Error());
      prepared[i] = std::move(allocated).Value();
    }
    allowed = co_await AuthorizeGoalDispatch(prepared[i].session, actor.Value(), goal);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  }
  allowed = co_await ValidateProjectSources(request.project, actor.Value(), request.inputs);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const auto principal = Actor(bound, Capability::CreateComparison);
  if (!principal.Succeeded()) co_return Result::Failure(principal.Error());
  if (principal.Value().principal != actor.Value().principal)
    co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (!source_current()) {
    co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Conflict, "Comparison history or authorization changed during preparation");
  }
  replay = co_await replay_command();
  if (!replay.Succeeded()) co_return Result::Failure(replay.Error());
  if (replay.Value()) co_return Result::Success(std::move(*replay.Value()));
  if (storage_error && options.store) co_return Result::Failure(*storage_error);
  std::set<SessionId> branches;
  std::map<WorkspaceId, std::size_t> workspaces;
  for (const auto& candidate : prepared) {
    const auto& target = candidate.session;
    if (sessions.contains(target.summary.id) || !branches.insert(target.summary.id).second ||
        (target.summary.workspace && WorkspaceRetired(*target.summary.workspace))) {
      co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Conflict, "A candidate workspace was taken or retired");
    }
    const auto dispatch = CheckGoalDispatch(target, goal);
    if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
    if (target.summary.workspace) ++workspaces[*target.summary.workspace];
  }
  for (std::size_t i = 0; i < prepared.size(); ++i) {
    if (request.candidates[i].workspace.kind != WorkspaceChoice::Kind::CopyFromSource) continue;
    const auto workspace = prepared[i].session.summary.workspace;
    if (!workspace || workspaces[*workspace] != 1 || std::ranges::any_of(sessions, [&](const auto& entry) {
          return entry.second.summary.workspace == workspace;
        })) co_return Fail<Receipt<ForkReviewCreated>>(ErrorCode::Conflict, "An isolated candidate must have its own workspace");
  }

  ForkReviewCreated created{.review = review.id};
  std::vector<const SessionData*> children;
  std::vector<RunId> launch;
  const SessionData* goal_home = nullptr;
  const auto started = Now();
  for (std::size_t i = 0; i < prepared.size(); ++i) {
    auto& proposed = prepared[i];
    const auto& candidate = request.candidates[i];
    const auto candidate_id = CandidateId{MakeId()};
    const auto run_id = RunId{MakeId()};
    const auto id = proposed.session.summary.id;
    auto budget = CandidateBudget(request.shared_budget, i, prepared.size());
    proposed.session.fork_branch = ForkBranchBinding{review.id, candidate_id, run_id, budget, started,
        candidate.workspace.kind == WorkspaceChoice::Kind::CopyFromSource};
    auto& session = sessions.emplace(id, std::move(proposed.session)).first->second;
    auto task = candidate.task;
    task.context = request.inputs;
    FirstTitle(session, task);
    auto& run = RecordRun(session, actor.Value().principal, std::move(task), proposed.start, RunTrigger::ForkComparison, {}, {}, run_id);
    if (budget.deadline) {
      const auto deadline = started + std::min(*budget.deadline, TimePoint::max() - started);
      if (!run.deadline || deadline < *run.deadline) run.deadline = deadline;
    }
    if (goal) goal_home = AttachGoalRun(run, goal);
    review.candidates.push_back({.id = candidate_id, .session = id, .run = run_id, .state = CandidateState::Running,
        .validation = CandidateVerification{.model = run.view.model.id, .workspace = session.summary.workspace,
            .conditions = request.comparison_criteria}});
    created.branches.push_back(id);
    children.push_back(&session);
    launch.push_back(run_id);
  }
  auto& project = projects.try_emplace(request.project, ProjectData{.id = request.project}).first->second;
  project.fork_reviews.emplace(review.id, std::move(review));
  ++project.revision;
  auto& first = sessions.at(children.front()->summary.id);
  children.erase(children.begin());
  if (goal_home) children.push_back(goal_home);
  auto commit = [&]() -> Async<Receipt<ForkReviewCreated>> {
    if (approved) co_return co_await FinishOperation(first, actor.Value().principal, command, key, created, true, true, &project, children);
    co_return co_await CommitProject(project, actor.Value(), command, std::move(input), created, &first, nullptr, {}, children);
  };
  const auto saved = co_await commit();
  if (saved.Succeeded() && stage == Stage::Started) {
    for (const auto& id : launch) {
      auto& run = runs.at(id);
      if (!run.stop.stop_requested() && !Terminal(run.view.state)) Launch(run);
    }
  }
  co_return saved;
}

Async<void> RuntimeState::AbandonComparisonWorkspace(CommandId command, WorkspaceCopyPlanRef copy, WorkspaceId allocated) {
  if (command.value.empty() || copy.id.value.empty() || !copy.version || allocated.value.empty())
    co_return Fail<void>(ErrorCode::InvalidArgument, "A comparison command, copy plan and allocated workspace are required");
  if (!options.store) co_return Fail<void>(ErrorCode::Unsupported, "Abandoning a candidate workspace requires durable storage");
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<void>::Failure(user.Error());
  const auto key = std::pair{user.Value(), command};
  const auto found = commands.find(key);
  if (found == commands.end() || !found->second.input.comparison || !found->second.plan || !found->second.plan->comparison)
    co_return Fail<void>(ErrorCode::NotFound, "No recoverable comparison command owns this worktree");
  auto& record = found->second;
  auto allowed = co_await WithSession(record.input.session, user.Value(), Capability::UpdateSession);
  if (!allowed.Succeeded()) co_return allowed;
  if (storage_error) co_return AgentResult<void>::Failure(*storage_error);
  const auto& request = *record.input.comparison;
  const auto item = std::ranges::find_if(request.candidates, [&](const auto& candidate) {
    return candidate.workspace.kind == WorkspaceChoice::Kind::CopyFromSource && candidate.workspace.copy_plan == copy;
  });
  if (item == request.candidates.end() || allocated == item->workspace.workspace)
    co_return Fail<void>(ErrorCode::Conflict, "The worktree does not belong to this comparison");
  const auto index = static_cast<std::size_t>(item - request.candidates.begin());
  const auto& branch = record.plan->comparison->candidates[index].session;
  const auto state = record.progress.state;
  if (state == CommandState::AwaitingApproval || state == CommandState::Scheduled || preparing_fork_reviews.contains(key) ||
      executing_operations.contains(key) || allocating_workspaces.contains(branch))
    co_return Fail<void>(ErrorCode::Busy, "The comparison is still being processed");
  if (state == CommandState::Applied || sessions.contains(branch) || std::ranges::any_of(sessions, [&](const auto& entry) {
        return entry.second.summary.workspace == allocated;
      })) co_return Fail<void>(ErrorCode::Conflict, "A committed conversation uses this candidate workspace");
  auto& home = sessions.at(record.input.session);
  const auto* source = Published(home);
  if (!source || source->summary.lifecycle == SessionLifecycle::Deleting || source->summary.workspace != item->workspace.workspace)
    co_return Fail<void>(ErrorCode::Conflict, "The candidate worktree no longer matches the source workspace");
  const auto prior = std::ranges::find(record.retired_comparison_workspaces, copy, &RetiredComparisonWorkspace::copy);
  if (prior != record.retired_comparison_workspaces.end()) {
    if (prior->workspace != allocated)
      co_return Fail<void>(ErrorCode::Conflict, "The copy plan was retired with a different workspace");
    const auto& entries = saved_sessions.at(home.summary.id).image.commands;
    const auto saved = std::ranges::find_if(entries, [&](const auto& command) {
      return command.principal == user.Value() && command.record.progress.accepted.command == key.second;
    });
    if (saved != entries.end() && std::ranges::find(saved->record.retired_comparison_workspaces, copy,
        &RetiredComparisonWorkspace::copy) != saved->record.retired_comparison_workspaces.end())
      co_return AgentResult<void>::Success();
  } else {
    if (WorkspaceRetired(allocated) || WorkspaceCopyRetired(copy))
      co_return Fail<void>(ErrorCode::Conflict, "Another record already retired this workspace");
    record.retired_comparison_workspaces.push_back({std::move(copy), std::move(allocated)});
    ++home.sequence;
  }
  co_return co_await WhenSaved(Persist(home));
}

Async<std::vector<ForkReviewSummary>> RuntimeState::ListForkReviews(SessionId source, std::optional<RunId> bound) {
  using Result = AgentResult<std::vector<ForkReviewSummary>>;
  const auto found = sessions.find(source);
  const auto* session = found == sessions.end() ? nullptr : Published(found->second);
  if (!session || session->summary.lifecycle == SessionLifecycle::Deleting || !session->summary.project)
    co_return Fail<std::vector<ForkReviewSummary>>(ErrorCode::NotFound, "Comparison source unavailable");
  const auto project = *session->summary.project;
  auto actor = co_await ProjectAccess(project, bound, Capability::ReadProject);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto allowed = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, found->second));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const auto* image = PublishedProject(project);
  if (!image) co_return Result::Success({});
  const auto revision = image->revision;
  const auto access = access_revision;
  std::vector<std::pair<Revision, ForkReviewSnapshot>> comparisons;
  const auto add_comparison = [&](const CommandProgress& progress) {
    if (progress.state != CommandState::Applied || !progress.result) return;
    const auto* created = std::get_if<ForkReviewCreated>(&*progress.result);
    const auto& stamp = progress.resolution ? *progress.resolution : progress.accepted;
    if (!created || stamp.project != project || !stamp.project_revision) return;
    const auto found = image->fork_reviews.find(created->review);
    if (found != image->fork_reviews.end() && found->second.request.from.session == source)
      comparisons.emplace_back(*stamp.project_revision, found->second);
  };
  for (const auto& command : image->commands) add_comparison(command.progress);
  if (const auto saved = saved_sessions.find(source); saved != saved_sessions.end()) {
    for (const auto& entry : saved->second.image.commands) add_comparison(entry.record.progress);
  } else if (!options.store) {
    for (const auto& [key, command] : commands) {
      if (command.progress.accepted.session == source) add_comparison(command.progress);
    }
  }
  std::ranges::sort(comparisons, std::ranges::greater{}, &std::pair<Revision, ForkReviewSnapshot>::first);
  std::vector<ForkReviewSummary> result;
  for (const auto& [sequence, review] : comparisons) {
    allowed = co_await Require(ReviewPolicy(*this, actor.Value(), review, Capability::ReadProject));
    if (!allowed.Succeeded()) {
      if (allowed.Error().code != ErrorCode::PermissionDenied && allowed.Error().code != ErrorCode::NotFound)
        co_return Result::Failure(allowed.Error());
      continue;
    }
    allowed = co_await ValidateProjectSources(project, actor.Value(), review.request.inputs);
    if (!allowed.Succeeded()) {
      if (allowed.Error().code != ErrorCode::PermissionDenied && allowed.Error().code != ErrorCode::NotFound)
        co_return Result::Failure(allowed.Error());
      continue;
    }
    result.push_back({review.id, review.revision, review.state, review.request.comparison_criteria});
  }
  auto current = Actor(bound, Capability::ReadProject);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != actor.Value().principal)
    co_return Fail<std::vector<ForkReviewSummary>>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (access != access_revision || PublishedProject(project)->revision != revision ||
      !Published(found->second) || Published(found->second)->summary.lifecycle == SessionLifecycle::Deleting)
    co_return Fail<std::vector<ForkReviewSummary>>(ErrorCode::CursorExpired, "Comparisons changed while reading");
  co_return Result::Success(std::move(result));
}

Async<ForkReviewSnapshot> RuntimeState::ReadForkReview(ForkReviewId id, std::optional<RunId> bound) {
  using Result = AgentResult<ForkReviewSnapshot>;
  const auto project = FindForkReviewProject(id);
  if (!project) co_return Fail<ForkReviewSnapshot>(ErrorCode::NotFound, "Comparison unavailable");
  auto actor = co_await ProjectAccess(*project, bound, Capability::ReadProject);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto view = PublishedProject(*project)->fork_reviews.at(id);
  const auto epoch = access_revision;
  auto allowed = co_await Require(ReviewPolicy(*this, actor.Value(), view, Capability::ReadProject));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const auto source = sessions.find(view.request.from.session);
  if (source == sessions.end() || source->second.summary.lifecycle == SessionLifecycle::Deleting)
    co_return Fail<ForkReviewSnapshot>(ErrorCode::NotFound, "Comparison history unavailable");
  allowed = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, source->second));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await ValidateProjectSources(*project, actor.Value(), view.request.inputs);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());

  std::vector<ForkCandidate> visible;
  std::vector<CandidateRequest> visible_requests;
  for (std::size_t i = 0; i < view.candidates.size(); ++i) {
    auto& candidate = view.candidates[i];
    const auto found = sessions.find(candidate.session);
    const auto* image = found == sessions.end() ? nullptr : Published(found->second);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting) continue;
    const auto discover = CollaborationAccess(actor.Value(), Capability::DiscoverSession, *image);
    const auto read = CollaborationAccess(actor.Value(), Capability::ReadSession, *image);
    allowed = co_await Require(discover);
    if (!allowed.Succeeded()) continue;
    allowed = co_await Require(read);
    if (!allowed.Succeeded()) continue;
    std::vector<ArtifactRef> artifacts;
    for (const auto& artifact : candidate.artifacts) {
      const std::vector<SourceSlice> sources{{artifact}};
      allowed = co_await ValidateProjectSources(*project, actor.Value(), sources);
      if (allowed.Succeeded()) artifacts.push_back(artifact);
    }
    candidate.artifacts = std::move(artifacts);
    if (candidate.validation && candidate.validation->evidence) {
      allowed = co_await ValidateProjectSources(*project, actor.Value(), candidate.validation->evidence->sources);
      if (!allowed.Succeeded()) candidate.validation->evidence.reset();
    }
    const auto session = sessions.find(candidate.session);
    const auto* current = session == sessions.end() ? nullptr : Published(session->second);
    if (!current || current->summary.lifecycle == SessionLifecycle::Deleting) continue;
    if (Active(candidate.state) && candidate.run) {
      if (options.store) {
        const auto& stored = saved_sessions.at(candidate.session).image;
        const auto saved = std::ranges::find_if(stored.runs, [&](const auto& run) { return run.view.id == candidate.run; });
        if (saved != stored.runs.end()) candidate.usage = saved->view.usage;
      } else candidate.usage = runs.at(*candidate.run).view.usage;
      if (current->fork_branch) candidate.elapsed = std::max(Duration::zero(), Now() - current->fork_branch->started);
    }
    visible.push_back(std::move(candidate));
    visible_requests.push_back(std::move(view.request.candidates[i]));
  }
  if (view.selected) {
    const auto candidate = std::ranges::find(visible, *view.selected, &ForkCandidate::id);
    const auto& adopted_in = PublishedProject(*project)->fork_adoptions;
    const auto binding = adopted_in.find(id);
    const auto target = binding == adopted_in.end() ? sessions.end() : sessions.find(binding->second.session);
    const auto command = binding == adopted_in.end() ? commands.end() :
        commands.find({binding->second.principal, binding->second.command});
    if (candidate == visible.end() || command == commands.end() || !command->second.input.adoption ||
        std::ranges::find(candidate->artifacts, command->second.input.adoption->candidate_artifact) == candidate->artifacts.end() ||
        target == sessions.end() ||
        target->second.summary.lifecycle == SessionLifecycle::Deleting ||
        !(co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, target->second))).Succeeded()) {
      view.selected.reset();
      view.adoption_call.reset();
      view.adoption_state.reset();
      view.adoption_reconciled = false;
    }
  }
  auto principal = Actor(bound, Capability::ReadProject);
  if (!principal.Succeeded()) co_return Result::Failure(principal.Error());
  if (principal.Value().principal != actor.Value().principal)
    co_return Fail<ForkReviewSnapshot>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (access_revision != epoch || PublishedProject(*project)->fork_reviews.at(id).revision != view.revision)
    co_return Fail<ForkReviewSnapshot>(ErrorCode::CursorExpired, "Comparison changed while reading");
  view.candidates = std::move(visible);
  view.request.candidates = std::move(visible_requests);
  co_return Result::Success(std::move(view));
}

Async<ArtifactRecord> RuntimeState::ReadAdoptionCandidate(const AdoptCandidateRequest& request, const ActionParty& actor) {
  using Result = AgentResult<ArtifactRecord>;
  const auto project = FindForkReviewProject(request.review);
  if (!project) co_return Fail<ArtifactRecord>(ErrorCode::NotFound, "Comparison unavailable");
  const auto review = PublishedProject(*project)->fork_reviews.at(request.review);
  const auto target = sessions.find(request.target_session);
  const auto candidate = std::ranges::find(review.candidates, request.candidate, &ForkCandidate::id);
  if (target == sessions.end() || target->second.summary.project != project ||
      (actor.session && sessions.at(*actor.session).summary.project != project) ||
      candidate == review.candidates.end() || request.target_session == candidate->session ||
      candidate->state != CandidateState::Ready ||
      std::ranges::find(candidate->artifacts, request.candidate_artifact) == candidate->artifacts.end()) {
    co_return Fail<ArtifactRecord>(ErrorCode::NotFound, "The selected version is unavailable for adoption in this project");
  }
  const auto source = sessions.find(review.request.from.session);
  const auto branch = sessions.find(candidate->session);
  if (source == sessions.end() || source->second.summary.lifecycle == SessionLifecycle::Deleting ||
      branch == sessions.end() || branch->second.summary.lifecycle == SessionLifecycle::Deleting) {
    co_return Fail<ArtifactRecord>(ErrorCode::NotFound, "Comparison history or branch unavailable");
  }
  const auto epoch = access_revision;
  auto allowed = co_await Require(ReviewPolicy(*this, actor, review, Capability::ReadProject));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await Require(CollaborationAccess(actor, Capability::ReadSession, source->second));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await Require(CollaborationAccess(actor, Capability::DiscoverSession, branch->second));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await ValidateProjectSources(*project, actor, review.request.inputs);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const std::vector<SourceSlice> selected{{request.candidate_artifact, request.parts}};
  allowed = co_await ValidateInputSources(target->second, actor, selected);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto artifact = co_await ReadArtifact(request.candidate_artifact, actor);
  if (!artifact.Succeeded()) co_return Result::Failure(artifact.Error());
  if (epoch != access_revision || artifact.Value().record.session != candidate->session ||
      artifact.Value().record.run != candidate->run) {
    co_return Fail<ArtifactRecord>(ErrorCode::Conflict, "The candidate or its authorization changed");
  }
  co_return Result::Success(std::move(artifact).Value().record);
}

AgentResult<void> RuntimeState::CheckAdoptionSlot(const SessionData& session, const AdoptCandidateRequest& request) const {
  const auto project = session.summary.project ? projects.find(*session.summary.project) : projects.end();
  if (project == projects.end()) return Fail<void>(ErrorCode::NotFound, "Comparison unavailable in the target project");
  const auto review = project->second.fork_reviews.find(request.review);
  if (review == project->second.fork_reviews.end()) return Fail<void>(ErrorCode::NotFound, "Comparison unavailable");
  const auto& view = review->second;
  if (view.revision != request.expected_review) return Fail<void>(ErrorCode::Conflict, "Comparison revision changed");
  if (view.state == ForkReviewState::Closed || view.adoption_state == ToolCallState::Succeeded)
    return Fail<void>(ErrorCode::AlreadyClosed, "Comparison closed or already applied");
  if (view.adoption_state && ActiveAdoption(*view.adoption_state)) return Fail<void>(ErrorCode::Busy, "A candidate is being adopted");
  if (view.adoption_state == ToolCallState::ResultUnknown ||
      (view.adoption_state == ToolCallState::PartiallyApplied && !view.adoption_reconciled))
    return Fail<void>(ErrorCode::ResultUnknown, "Reconcile the earlier adoption before preparing another one");
  if (session.fork_branch && !session.fork_branch->isolated)
    return Fail<void>(ErrorCode::PermissionDenied, "The target conversation only stages candidates");
  if (options.store && session.retention != SessionRetention::Stored)
    return Fail<void>(ErrorCode::InvalidArgument, "An adoption in a stored project needs a stored target conversation");
  return AgentResult<void>::Success();
}

bool RuntimeState::AdoptionBound(const SessionData& session, const AdoptCandidateRequest& request, const ToolCallId& call) const {
  if (!session.summary.project || !projects.contains(*session.summary.project)) return false;
  const auto& project = projects.at(*session.summary.project);
  const auto found = project.fork_reviews.find(request.review);
  return found != project.fork_reviews.end() && found->second.selected == request.candidate &&
      found->second.adoption_call == call && found->second.state != ForkReviewState::Closed;
}

ProjectData* RuntimeState::RefreshForkAdoptions(const SessionData& session) {
  if (!session.summary.project || !projects.contains(*session.summary.project)) return nullptr;
  auto& project = projects.at(*session.summary.project);
  bool changed = false;
  for (const auto& [id, binding] : project.fork_adoptions) {
    if (binding.session != session.summary.id) continue;
    auto& review = project.fork_reviews.at(id);
    const auto command = commands.find({binding.principal, binding.command});
    if (command == commands.end()) continue;
    const auto tool = std::ranges::find(session.tools, *review.adoption_call, &ToolCallSnapshot::id);
    const auto& progress = command->second.progress;
    auto current = ToolCallState::Preparing;
    if (tool != session.tools.end()) current = tool->state;
    else if (progress.state == CommandState::Failed) {
      current = progress.failure && progress.failure->code == ErrorCode::Conflict ? ToolCallState::Conflict : ToolCallState::Failed;
    } else if (progress.state == CommandState::Withdrawn) current = ToolCallState::Cancelled;
    const bool reconciled = tool != session.tools.end() && tool->reconciliation.has_value();
    if ((review.adoption_state == current && review.adoption_reconciled == reconciled) ||
        (!ActiveAdoption(current) && progress.state == CommandState::Scheduled)) continue;
    review.adoption_state = current;
    review.adoption_reconciled = reconciled;
    ++review.revision;
    changed = true;
  }
  if (!changed) return nullptr;
  ++project.revision;
  return &project;
}

Async<CommandReceipt> RuntimeState::CloseForkReviewAsync(CommandId command, CloseForkReview request, std::optional<RunId> bound) {
  using Result = AgentResult<CommandReceipt>;
  const auto id = FindForkReviewProject(request.review);
  if (!id) co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Comparison unavailable");
  auto actor = co_await ProjectAccess(*id, bound, Capability::CreateComparison);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  const auto epoch = access_revision;
  const auto& published = PublishedProject(*id)->fork_reviews.at(request.review);
  auto allowed = co_await Require(ReviewPolicy(*this, actor.Value(), published, Capability::CreateComparison));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto current = Actor(bound, Capability::CreateComparison);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != actor.Value().principal || epoch != access_revision)
    co_return Fail<CommandReceipt>(ErrorCode::PermissionDenied, "Comparison authorization changed");
  ProjectCommand input{request};
  auto replay = ReplayProject<std::monostate>(actor.Value(), *id, command, input);
  if (!replay.Succeeded()) co_return Result::Failure(replay.Error());
  if (replay.Value()) co_return co_await ReplayReceipt(Result::Success(*replay.Value()));
  if (storage_error && options.store) co_return Result::Failure(*storage_error);
  auto& project = projects.at(*id);
  auto& view = project.fork_reviews.at(request.review);
  if (view.revision != request.expected) co_return Fail<CommandReceipt>(ErrorCode::Conflict, "Comparison revision changed");
  if (view.state == ForkReviewState::Closed) co_return Fail<CommandReceipt>(ErrorCode::AlreadyClosed, "Comparison closed");
  if (view.adoption_state && ActiveAdoption(*view.adoption_state))
    co_return Fail<CommandReceipt>(ErrorCode::Busy, "Finish or withdraw the pending adoption before closing the comparison");
  view.state = ForkReviewState::Closed;
  ++view.revision;
  ++project.revision;
  co_return co_await CommitProject(project, actor.Value(), std::move(command), std::move(input), std::monostate{});
}

ProjectData* RuntimeState::FinishForkCandidate(const RunSnapshot& run) {
  const auto& session = sessions.at(run.session);
  if (!session.fork_branch || session.fork_branch->run != run.id || !session.summary.project || !Terminal(run.state)) return nullptr;
  const auto& branch = *session.fork_branch;
  auto& project = projects.at(*session.summary.project);
  auto& review = project.fork_reviews.at(branch.review);
  auto candidate = std::ranges::find(review.candidates, branch.candidate, &ForkCandidate::id);
  if (candidate == review.candidates.end() || !Active(candidate->state)) return nullptr;
  candidate->state = run.state == RunState::Succeeded ? CandidateState::Ready :
      run.state == RunState::Failed ? CandidateState::Failed : CandidateState::Cancelled;
  candidate->usage = run.usage;
  candidate->elapsed = std::max(Duration::zero(), Now() - branch.started);
  candidate->reason = run.failure;
  if (run.result) {
    for (const auto& ref : run.result->artifacts) {
      if (std::ranges::any_of(session.artifacts, [&](const auto& artifact) {
            return artifact.record.metadata.ref == ref && artifact.record.run == run.id &&
                artifact.record.availability == ArtifactAvailability::Available;
          }) && std::ranges::find(candidate->artifacts, ref) == candidate->artifacts.end()) candidate->artifacts.push_back(ref);
    }
    if (candidate->validation && run.result->structured) {
      std::vector<SourceSlice> sources;
      for (const auto& ref : candidate->artifacts) sources.push_back({ref});
      candidate->validation->evidence = Evidence{*run.result->structured, std::move(sources)};
    }
  }
  if (review.state == ForkReviewState::Running && std::ranges::none_of(review.candidates, [](const auto& candidate) { return Active(candidate.state); })) {
    review.state = ForkReviewState::Ready;
  }
  ++review.revision;
  ++project.revision;
  return &project;
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

ForkReviewService::ForkReviewService(std::shared_ptr<detail::RuntimeState> runtime, std::optional<RunId> actor)
    : runtime_(std::move(runtime)), actor_(std::move(actor)) {}

Async<Receipt<ForkReviewCreated>> ForkReviewService::CreateComparisonAsync(CommandId command, ForkReviewRequest request) {
  return [](ForkReviewService service, CommandId id, ForkReviewRequest value) -> Async<Receipt<ForkReviewCreated>> {
    co_return co_await service.runtime_->CreateForkReview(std::move(id), std::move(value), service.actor_);
  }(*this, std::move(command), std::move(request));
}

Async<ForkReviewSnapshot> ForkReviewService::ReadComparisonAsync(ForkReviewId review) {
  return [](ForkReviewService service, ForkReviewId id) -> Async<ForkReviewSnapshot> {
    co_return co_await service.runtime_->ReadForkReview(std::move(id), service.actor_);
  }(*this, std::move(review));
}

Async<std::vector<ForkReviewSummary>> ForkReviewService::ListComparisonsAsync(SessionId source) {
  return [](ForkReviewService service, SessionId id) -> Async<std::vector<ForkReviewSummary>> {
    co_return co_await service.runtime_->ListForkReviews(std::move(id), service.actor_);
  }(*this, std::move(source));
}

Async<Subscription> ForkReviewService::ObserveComparisonsAsync(SessionId source, Observer<std::vector<ForkReviewSummary>> observer) {
  return detail::RuntimeState::Observe(runtime_, std::move(observer),
      [id = std::move(source), actor = actor_](detail::RuntimeState& runtime) { return runtime.ListForkReviews(id, actor); });
}

Async<Subscription> ForkReviewService::ObserveComparisonAsync(ForkReviewId review, Observer<ForkReviewSnapshot> observer) {
  return detail::RuntimeState::Observe(runtime_, std::move(observer),
      [id = std::move(review), actor = actor_](detail::RuntimeState& runtime) { return runtime.ReadForkReview(id, actor); });
}

Async<Receipt<ChangePlanAccepted>> ForkReviewService::PrepareAdoptionAsync(CommandId command, AdoptCandidateRequest request) {
  return [](ForkReviewService service, CommandId id, AdoptCandidateRequest value) -> Async<Receipt<ChangePlanAccepted>> {
    detail::CommandInput input{.operation = detail::Operation::AdoptCandidate, .session = value.target_session,
        .run = service.actor_.value_or(RunId{}), .adoption = std::move(value)};
    co_return co_await service.runtime_->PrepareArtifactChange(std::move(id), std::move(input));
  }(*this, std::move(command), std::move(request));
}

Async<CommandReceipt> ForkReviewService::CloseComparisonAsync(CommandId command, ForkReviewId review, Revision expected) {
  return [](ForkReviewService service, CommandId id, ForkReviewId review, Revision expected) -> Async<CommandReceipt> {
    detail::CloseForkReview request{std::move(review), expected};
    co_return co_await service.runtime_->CloseForkReviewAsync(std::move(id), std::move(request), service.actor_);
  }(*this, std::move(command), std::move(review), expected);
}

Async<void> ForkReviewService::AbandonComparisonWorkspaceAsync(CommandId comparison, WorkspaceCopyPlanRef copy,
    WorkspaceId allocated) {
  return [](ForkReviewService service, CommandId id, WorkspaceCopyPlanRef plan, WorkspaceId workspace) -> Async<void> {
    if (service.actor_) co_return detail::Fail<void>(ErrorCode::PermissionDenied, "Workspace recovery is a host operation");
    co_return co_await service.runtime_->AbandonComparisonWorkspace(std::move(id), std::move(plan), std::move(workspace));
  }(*this, std::move(comparison), std::move(copy), std::move(allocated));
}

ForkReviewService AgentRuntime::ForkReviews() { return ForkReviewService(state_); }

ForkReviewService RunContext::ForkReviews() { return ForkReviewService(runtime_, run_); }

} // namespace huxerui::agentkit
