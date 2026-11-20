#pragma once

/// @file
/// @brief Session-level goals, scheduling, definition changes and completion review.
/// Use Conversation::Goals() for user operations or RunContext::Goals() for a bound agent. ProjectService handles
/// separate task dependencies. Host verification is registered with RuntimeOptions::verify.

#include <huxerui/agentkit/project.h>

namespace huxerui::agentkit {

/// @brief Controls whether evidence verification is sufficient to complete a goal.
enum class GoalCompletionPolicy {
  /// A passed GoalVerifier result can complete the goal.
  Verified,
  /// A passed verification opens a version-bound user approval.
  RequireConfirmation
};

/// @brief Versioned objective and evidence requirements for a goal.
struct GoalDefinition {
  /// Human-readable objective included in the goal's execution context.
  std::string objective;
  /// Optional schema applied to Evidence::values before verification.
  std::optional<JsonSchema> evidence_schema;
  /// Structured completion criteria supplied to GoalVerifier.
  JsonValue completion_conditions;
  /// Policy for closing a goal after verification.
  GoalCompletionPolicy completion = GoalCompletionPolicy::Verified;
  /// Compares the stored fields for equality.
  bool operator==(const GoalDefinition&) const = default;
};

/// @brief Initial goal definition and cumulative execution budget.
struct GoalSpec {
  /// Objective and completion policy.
  GoalDefinition definition;
  /// Budget shared by work attributed to the goal.
  Budget budget;
  /// Compares the stored fields for equality.
  bool operator==(const GoalSpec&) const = default;
};

/// @brief Lifecycle of a goal independently of the session's current RunState.
enum class GoalState {
  /// Recorded and waiting for activation.
  Proposed,
  /// Eligible for scheduling when the session and dependencies are ready.
  Active,
  /// Waiting for a prerequisite or a new actionable outcome.
  Blocked,
  /// Automatic advancement is suspended.
  Paused,
  /// The goal has exhausted a configured budget.
  BudgetLimited,
  /// The runtime is checking submitted evidence.
  Verifying,
  /// Verified evidence is waiting for user confirmation.
  AwaitingCompletionApproval,
  /// The completion conditions and any required review were accepted.
  Completed,
  /// The goal has been closed by cancellation.
  Cancelled
};

/// @brief The pending human review for one verified definition and evidence revision.
struct GoalCompletionReview {
  /// Interaction to submit through Conversation::SubmitAsync().
  PendingApproval approval;
  /// Exact goal definition being approved.
  GoalRef definition;
  /// Verification revision bound to the approval.
  Revision verification_revision;
};

/// @brief Current visible goal state published by SessionGoals.
struct GoalSnapshot {
  /// Stable goal ID and current definition version.
  GoalRef ref;
  /// Session that owns the goal.
  SessionId session;
  /// Mutable state revision to use for ChangeGoal::expected.
  Revision revision;
  /// Current definition and budget.
  GoalSpec spec;
  /// Current scheduling or review state.
  GoalState state;
  /// Usage accumulated for the goal.
  Usage usage;
  /// Project tasks associated with the goal.
  std::vector<ProjectTaskId> tasks;
  /// Runs recorded for the goal.
  std::vector<RunId> runs;
  /// Current submitted evidence, if any.
  std::optional<Evidence> evidence;
  /// Most recent result for the current evidence and definition, when available.
  std::optional<VerificationResult> verification;
  /// Pending completion approval, when required and still current.
  std::optional<GoalCompletionReview> completion_review;
  /// Visible explanation of the current blocked, limited or terminal state, if supplied.
  std::optional<std::string> reason;
};

/// @brief Creates a proposed goal for the session.
struct ProposeGoal {
  /// Initial definition and budget.
  GoalSpec spec;
  /// Compares the stored fields for equality.
  bool operator==(const ProposeGoal&) const = default;
};
/// @brief Updates the definition of an existing goal.
struct EditGoal {
  /// Replacement objective and completion requirements.
  GoalDefinition definition;
  /// Token returned by SessionGoals::PreviewEditAsync() for the proposed edit, when required.
  std::optional<std::string> preview_token;
  /// Compares the stored fields for equality.
  bool operator==(const EditGoal&) const = default;
};
/// @brief Activates a proposed goal and allows scheduling.
struct EnableGoal {
  /// Compares the stored fields for equality.
  bool operator==(const EnableGoal&) const = default;
};
/// @brief Suspends automatic advancement of the goal.
struct PauseGoal {
  /// Compares the stored fields for equality.
  bool operator==(const PauseGoal&) const = default;
};
/// @brief Makes a paused goal eligible to advance again.
struct ResumeGoal {
  /// Compares the stored fields for equality.
  bool operator==(const ResumeGoal&) const = default;
};
/// @brief Closes the goal and its pending completion work.
struct CancelGoal {
  /// Compares the stored fields for equality.
  bool operator==(const CancelGoal&) const = default;
};
/// @brief Changes the goal's cumulative budget.
struct SetGoalBudget {
  /// Replacement budget. Previously accumulated usage still applies.
  Budget budget;
  /// Compares the stored fields for equality.
  bool operator==(const SetGoalBudget&) const = default;
};
/// @brief Replaces the goal's set of linked project tasks.
struct SetGoalTasks {
  /// Project task IDs to associate with the goal.
  std::vector<ProjectTaskId> tasks;
  /// Compares the stored fields for equality.
  bool operator==(const SetGoalTasks&) const = default;
};
/// @brief Submits version-bound evidence for verification.
struct SubmitGoalEvidence {
  /// Exact definition to verify against.
  GoalRef goal;
  /// Structured values and source refs to validate.
  Evidence evidence;
  /// Compares the stored fields for equality.
  bool operator==(const SubmitGoalEvidence&) const = default;
};

/// @brief Operations allowed on an existing GoalId.
using GoalChange = std::variant<EditGoal, EnableGoal, PauseGoal, ResumeGoal, CancelGoal,
    SetGoalBudget, SetGoalTasks, SubmitGoalEvidence>;

/// @brief Optimistic update to an existing goal.
struct ChangeGoal {
  /// Goal to change.
  GoalId goal;
  /// Expected GoalSnapshot::revision.
  Revision expected;
  /// Operation to apply.
  GoalChange change;
  /// Compares the stored fields for equality.
  bool operator==(const ChangeGoal&) const = default;
};
/// @brief A proposal or a revision-checked goal update.
using GoalCommand = std::variant<ProposeGoal, ChangeGoal>;

/// @brief View and commands for the goal of one session.
class SessionGoals {
public:
  /// @brief Reads the currently visible goal, or an empty value when the session has none.
  /// @return The visible goal snapshot, or an empty value when the session has no goal.
  Async<std::optional<GoalSnapshot>> ReadAsync();

  /// @brief Reads the definition bound to the exact GoalRef.
  /// @param goal Goal identity and definition revision to resolve.
  /// @return The definition recorded for that revision.
  Async<GoalDefinition> ReadDefinitionAsync(GoalRef goal);

  /// @brief Computes the impact of replacing the definition and returns the confirmation token.
  /// @param goal Goal to edit.
  /// @param expected Current expected revision.
  /// @param definition Proposed replacement definition.
  /// @return The effect of the proposed edit for review before submission.
  Async<GoalEditPreview> PreviewEditAsync(GoalId goal, Revision expected, GoalDefinition definition);

  /// @brief Observes changes to the visible goal, including an empty current goal.
  /// @param observer Callback receiving the current goal, an empty snapshot, or an error.
  /// @return A subscription that keeps the goal observation registered.
  Async<Subscription> ObserveAsync(Observer<std::optional<GoalSnapshot>> observer);

  /// @brief Submits a creation or state change under a stable command ID.
  /// @param command Stable ID for this goal mutation.
  /// @param request Goal operation and its revision preconditions.
  /// @return Receipt identifying the updated goal and revision.
  Async<Receipt<GoalUpdated>> UpdateAsync(CommandId command, GoalCommand request);

private:
  SessionGoals(std::shared_ptr<detail::RuntimeState> runtime, SessionId session,
               PrincipalId principal, std::optional<RunId> actor = {});
  std::shared_ptr<detail::RuntimeState> runtime_;
  SessionId session_;
  PrincipalId principal_;
  std::optional<RunId> actor_;
  friend class Conversation;
  friend class RunContext;
};

} // namespace huxerui::agentkit
