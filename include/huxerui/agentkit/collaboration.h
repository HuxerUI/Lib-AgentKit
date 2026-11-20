#pragma once

/// @file
/// @brief Messages, deliveries, replies and waits between independent sessions.
/// Use AgentRuntime::Collaboration() for user-side access or RunContext::Collaboration() for a bound sender.
/// Use RunContext::DelegateAsync() for children that share the parent's session and cancellation.

#include <huxerui/agentkit/session.h>

namespace huxerui::agentkit {

/// @brief A message placed in the receiving session's inbox without starting a Run.
struct Notice {
  /// Text and authorized references sent with the notice.
  SendInput input;
  /// Compares the stored fields for equality.
  bool operator==(const Notice&) const = default;
};

/// @brief Supplement targeted at an exact Run in another session.
struct SteerRequest {
  /// Active Run that must still match when the supplement is applied.
  RunId expected_run;
  /// Supplemental text and input references.
  SendInput input;
  /// Compares the stored fields for equality.
  bool operator==(const SteerRequest&) const = default;
};

/// @brief An outbound collaboration message accepted by SessionCollaboration.
struct SessionMessageRequest {
  /// Receiving session.
  SessionId to;
  /// A notice, WorkRequest or targeted supplement.
  std::variant<Notice, WorkRequest, SteerRequest> body;
  /// Related collaboration message, if this is a follow-up.
  std::optional<CollaborationMessageId> related;
  /// Absolute delivery deadline. Empty uses the runtime's applicable limits.
  std::optional<TimePoint> deliver_by;
  /// Compares the stored fields for equality.
  bool operator==(const SessionMessageRequest&) const = default;
};

/// @brief Business status of a recorded SessionReply.
enum class ReplyStatus {
  /// The receiving side reports completion.
  Succeeded,
  /// The receiving side reports failure.
  Failed,
  /// The receiving side explicitly declines the requested work.
  Refused
};

/// @brief Final answer submitted against a WorkRequest that expects a reply.
struct SessionReply {
  /// Business outcome of the reply.
  ReplyStatus status;
  /// Human-readable result summary.
  std::string summary;
  /// Structured result checked against ReplyExpectation::schema when present.
  std::optional<JsonValue> value;
  /// Exact artifact versions delivered as part of the reply.
  std::vector<ArtifactRef> artifacts;
  /// Compares the stored fields for equality.
  bool operator==(const SessionReply&) const = default;
};

/// @brief Why a collaboration reply wait resolved.
enum class ReplyEnd {
  /// A final reply was received. Inspect SessionReply::status for the business outcome.
  Received,
  /// The request was denied or could not be delivered.
  Rejected,
  /// The request or its reply deadline expired.
  Expired,
  /// The message was withdrawn before the reply completed.
  Withdrawn,
  /// The target Run failed before returning the required reply.
  TargetFailed,
  /// The target Run was cancelled before returning the required reply.
  TargetCancelled,
  /// The target Run finished without the required reply.
  MissingReply,
  /// This wait reached its own deadline.
  WaitTimedOut,
  /// Reserved outcome for a cancelled wait. A cancelled local task currently stops waiting without a result.
  WaitCancelled,
  /// A previously registered wait became part of a dependency cycle.
  WaitCycle
};

/// @brief Resolution of a single reply wait.
struct ReplyOutcome {
  /// Original collaboration request ID.
  CollaborationMessageId request;
  /// Terminal reason for the wait.
  ReplyEnd end;
  /// Received final reply, when available and visible.
  std::optional<SessionReply> reply;
  /// Target Run that accepted the work, when available and visible.
  std::optional<RunId> target_run;
};

/// @brief Optional controls for reply and project task waits.
struct WaitOptions {
  /// Absolute deadline for this wait, independent of the target work.
  std::optional<TimePoint> deadline;
};

/// @brief Permission-filtered delivery record returned by ReadMessageAsync().
struct CollaborationRecord {
  /// Addressing, delivery status and target Run summary.
  CollaborationSummary summary;
  /// Request body when the message is a request and the body is visible.
  std::optional<SessionMessageRequest> request;
  /// Reply body when recorded and visible.
  std::optional<SessionReply> reply;
  /// Known result of the original reply expectation, when applicable.
  std::optional<ReplyOutcome> outcome;
  /// Denial, withdrawal, expiry or related diagnostic, when retained.
  std::optional<AgentError> reason;
};

/// @brief Sends and observes collaboration operations under a user or Run binding.
class SessionCollaboration {
public:
  /// @brief Accepts a notice, work request or supplement and returns its delivery receipt.
  /// @param command Stable ID for this logical message.
  /// @param request Target session, purpose and authorized content.
  /// @return A receipt identifying the accepted message.
  Async<Receipt<MessageAccepted>> SendMessageAsync(CommandId command, SessionMessageRequest request);

  /// @brief Submits one final reply under the original collaboration request ID.
  /// @param command Stable ID for this logical reply.
  /// @param request Original request message to answer.
  /// @param reply Reply outcome and optional supporting content.
  /// @return A receipt identifying the accepted reply.
  Async<Receipt<MessageAccepted>> ReplyAsync(CommandId command, CollaborationMessageId request, SessionReply reply);

  /// @brief Starts a queued work request from the receiving side when authorization and session state permit it.
  /// @param command Stable ID for the start request.
  /// @param request Work request accepted by this session.
  /// @return A receipt identifying the started request.
  Async<Receipt<MessageAccepted>> StartRequestAsync(CommandId command, CollaborationMessageId request);

  /// @brief Withdraws a collaboration message that has not become effective.
  /// @param command Stable ID for this withdrawal.
  /// @param message ID of the outstanding message.
  /// @return The withdrawal receipt or a lifecycle error when work has already begun.
  Async<CommandReceipt> WithdrawMessageAsync(CommandId command, CollaborationMessageId message);

  /// @brief Reads the current delivery record using the caller's current access.
  /// @param message ID issued in the original message receipt.
  /// @return The collaboration record accessible to this actor.
  Async<CollaborationRecord> ReadMessageAsync(CollaborationMessageId message);

  /// @brief Returns an existing reply or registers a wait for the original request.
  /// A Run-bound wait contributes an edge to the runtime's wait graph.
  /// @param request Original request expecting a reply.
  /// @param options Optional absolute deadline for the wait.
  /// @return The settled reply outcome.
  /// @note The wait deadline does not cancel the receiving session's work.
  Async<ReplyOutcome> WaitForReplyAsync(CollaborationMessageId request, WaitOptions options = {});

private:
  SessionCollaboration(std::shared_ptr<detail::RuntimeState> runtime, std::optional<RunId> run = {});
  std::shared_ptr<detail::RuntimeState> runtime_;
  std::optional<RunId> run_;
  friend class AgentRuntime;
  friend class RunContext;
};

/// @brief Receiving host's initial disposition of an incoming collaboration payload.
enum class ReceiveAction {
  /// Allows delivery subject to scope and runtime authorization checks.
  Allow,
  /// Holds work for receiving-side action.
  Hold,
  /// Rejects the incoming payload.
  Deny
};

/// @brief Context passed to the receiving host's ReceivePolicy.
struct ReceiveRequest {
  /// Authenticated sender, including Run identity when applicable.
  ActionParty sender;
  /// Receiving session's current summary.
  SessionSummary target;
  /// Stable collaboration message ID.
  CollaborationMessageId message;
  /// Incoming request or reply that the receiver is deciding on.
  std::variant<SessionMessageRequest, SessionReply> payload;
  /// Associated goal version, when applicable.
  std::optional<GoalRef> goal;
};

/// @brief Receiving policy result applied in addition to access policy.
struct ReceiveDecision {
  /// Delivery disposition.
  ReceiveAction action;
  /// Allows eligible work to start without manual receiving-side action.
  bool allow_auto_start = false;
  /// Authorized work scope to intersect with the requested scope, when supplied.
  std::optional<WorkScope> allowed_work_scope;
  /// Human-readable rationale, when supplied.
  std::string reason;
};

/// @brief Host callback that decides whether a receiving session can accept the payload.
using ReceivePolicy = std::function<Async<ReceiveDecision>(ReceiveRequest)>;

} // namespace huxerui::agentkit
