#pragma once

/// @file
/// @brief Forked alternatives, comparison snapshots, adoption and workspace recovery.
/// Use ForkReviewService to start isolated candidates at one HistoryBoundary, observe their results and prepare adoption
/// of an exact artifact version through the tool approval path.

#include <huxerui/agentkit/artifact.h>
#include <huxerui/agentkit/project.h>

namespace huxerui::agentkit {

/// @brief One candidate branch in a ForkReviewRequest.
struct CandidateRequest {
  /// Agent and session configuration for the candidate.
  SessionOptions session;
  /// Existing or preplanned copied workspace.
  WorkspaceChoice workspace;
  /// Input used to start the candidate's initial Run.
  SendInput task;
  /// Compares the stored fields for equality.
  bool operator==(const CandidateRequest&) const = default;
};

/// @brief Creates a comparison from the same versioned baseline and input set.
struct ForkReviewRequest {
  /// Committed source session boundary shared by the candidates.
  HistoryBoundary from;
  /// Project that owns the comparison.
  ProjectId project;
  /// Versioned sources common to the comparison.
  std::vector<SourceSlice> inputs;
  /// Requested branches in presentation order.
  std::vector<CandidateRequest> candidates;
  /// Host-defined criteria presented with candidate evidence.
  JsonValue comparison_criteria;
  /// Budget split across the candidate runs.
  Budget shared_budget;
  /// Compares the stored fields for equality.
  bool operator==(const ForkReviewRequest&) const = default;
};

/// @brief Observable execution state of a candidate branch.
enum class CandidateState {
  /// Accepted candidate with work waiting to start.
  Queued,
  /// Initial candidate Run is active.
  Running,
  /// Candidate Run completed successfully.
  Ready,
  /// Candidate Run failed.
  Failed,
  /// Candidate Run was cancelled.
  Cancelled
};
/// @brief Lifecycle of a comparison independently of any one candidate.
enum class ForkReviewState {
  /// At least one candidate is still settling.
  Running,
  /// Candidate execution has settled and results can be compared.
  Ready,
  /// The comparison has been closed for further adoption.
  Closed
};

/// @brief Directory entry for a comparison visible from its source session.
struct ForkReviewSummary {
  /// Comparison ID for detail reads.
  ForkReviewId id;
  /// Current comparison revision.
  Revision revision;
  /// Current comparison lifecycle.
  ForkReviewState state;
  /// Criteria visible in the comparison directory.
  JsonValue criteria;
  /// Compares the stored fields for equality.
  bool operator==(const ForkReviewSummary&) const = default;
};

/// @brief Model, workspace and evidence associated with evaluating a candidate.
struct CandidateVerification {
  /// Model registration ID used for the candidate, when known.
  std::optional<ModelId> model;
  /// Workspace bound to the candidate, when present.
  std::optional<WorkspaceId> workspace;
  /// Optional external workspace revision. The current comparison runtime does not populate it.
  std::optional<std::string> workspace_revision;
  /// Criteria against which the candidate is presented.
  JsonValue conditions;
  /// Versioned evidence available from the candidate outcome.
  std::optional<Evidence> evidence;
};

/// @brief Permission-filtered state of one candidate branch.
struct ForkCandidate {
  /// Candidate identifier within the comparison.
  CandidateId id;
  /// Session that owns the branch.
  SessionId session;
  /// Initial Run associated with the candidate, when available.
  std::optional<RunId> run;
  /// Current candidate execution state.
  CandidateState state;
  /// Visible artifact versions produced in the branch.
  std::vector<ArtifactRef> artifacts;
  /// Candidate verification details, when available.
  std::optional<CandidateVerification> validation;
  /// Reported usage of the candidate Run.
  Usage usage{};
  /// Wall-clock time attributed to the candidate.
  Duration elapsed{};
  /// Failure or cancellation diagnostic, when available.
  std::optional<AgentError> reason;
};

/// @brief Detailed comparison view and the latest adoption state visible to the caller.
struct ForkReviewSnapshot {
  /// Comparison ID.
  ForkReviewId id;
  /// Expected revision to use for adoption or close.
  Revision revision;
  /// Current comparison lifecycle.
  ForkReviewState state;
  /// Original comparison request after authorization projection.
  ForkReviewRequest request;
  /// Visible candidate results in request order.
  std::vector<ForkCandidate> candidates;
  /// Candidate selected for the current adoption, when present.
  std::optional<CandidateId> selected;
  /// Tool call that owns the prepared adoption, when present.
  std::optional<ToolCallId> adoption_call;
  /// Visible execution state of that adoption call.
  std::optional<ToolCallState> adoption_state;
  /// Whether that call's previously unknown effects were reconciled.
  bool adoption_reconciled = false;
};

/// @brief User- or Run-bound service for comparison operations.
class ForkReviewService {
public:
  /// @brief Submits candidates from a common source boundary. May return PendingApproval.
  /// @param command Stable comparison ID used for replay and workspace recovery.
  /// @param request Source boundary and candidates to create.
  Async<Receipt<ForkReviewCreated>> CreateComparisonAsync(CommandId command, ForkReviewRequest request);

  /// @brief Lists visible comparisons associated with the source session.
  /// @param source Source session of the comparisons.
  /// @return Visible comparisons for that session.
  Async<std::vector<ForkReviewSummary>> ListComparisonsAsync(SessionId source);

  /// @brief Observes changes to the source session's comparison directory.
  /// @param source Source session of the comparisons.
  /// @param observer Callback receiving immutable summaries or an error.
  /// @return A subscription that keeps the list observation registered.
  Async<Subscription> ObserveComparisonsAsync(SessionId source, Observer<std::vector<ForkReviewSummary>> observer);

  /// @brief Reads the authorized projection of one comparison.
  /// @param review Comparison ID from a creation receipt or listing.
  /// @return The authorized comparison and visible candidate state.
  Async<ForkReviewSnapshot> ReadComparisonAsync(ForkReviewId review);

  /// @brief Observes the authorized comparison and adoption state.
  /// @param review Comparison to observe.
  /// @param observer Callback receiving immutable comparison snapshots or an error.
  /// @return A subscription that keeps the comparison observation registered.
  Async<Subscription> ObserveComparisonAsync(ForkReviewId review, Observer<ForkReviewSnapshot> observer);

  /// @brief Prepares a version-bound adoption and returns its Run, queue or approval receipt.
  /// @param command Stable ID of this adoption request.
  /// @param request Selected candidate and expected target versions.
  /// @return Plan receipt and any required approval.
  Async<Receipt<ChangePlanAccepted>> PrepareAdoptionAsync(CommandId command, AdoptCandidateRequest request);

  /// @brief Closes the comparison if expected still matches its current revision.
  /// @param command Stable ID of this close request.
  /// @param review Comparison to close.
  /// @param expected Current comparison revision.
  /// @return The receipt or a conflict if the comparison changed.
  Async<CommandReceipt> CloseComparisonAsync(CommandId command, ForkReviewId review, Revision expected);

  /// @brief Releases an uncommitted candidate workspace allocation after checking the original comparison and copy plan.
  /// @param comparison Original comparison creation command.
  /// @param copy Original copy plan for this candidate.
  /// @param allocated Workspace allocated for that copy.
  /// @note The runtime checks ownership before releasing the unused allocation.
  Async<void> AbandonComparisonWorkspaceAsync(CommandId comparison, WorkspaceCopyPlanRef copy, WorkspaceId allocated);

private:
  ForkReviewService(std::shared_ptr<detail::RuntimeState> runtime, std::optional<RunId> actor = {});
  std::shared_ptr<detail::RuntimeState> runtime_;
  std::optional<RunId> actor_;
  friend class AgentRuntime;
  friend class RunContext;
};

} // namespace huxerui::agentkit
