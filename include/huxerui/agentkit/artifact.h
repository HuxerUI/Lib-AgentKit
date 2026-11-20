#pragma once

/// @file
/// @brief Versioned artifacts, source-impact queries and approved changes.
/// Use Conversation for authorization scoped to an open session, or open ArtifactChanges through
/// AgentRuntime::Artifacts() or RunContext::Artifacts().
/// Integrate host storage through ArtifactStore and supply ChangeAdapters to prepare rework, revert and adoption plans.

#include <huxerui/agentkit/tool.h>

namespace huxerui::agentkit {

/// @brief Publication availability of an exact artifact version.
enum class ArtifactAvailability {
  /// The version is registered as available.
  Available,
  /// Represents missing backing content. Runtime publication currently produces Available.
  Missing,
  /// Represents a withdrawn version. Source-change records currently track withdrawal separately.
  Withdrawn
};

/// @brief Persisted publication metadata and its versioned provenance.
struct ArtifactRecord {
  /// Exact version, type, display name and optional byte length.
  ArtifactMetadata metadata;
  /// Originating session.
  SessionId session;
  /// Run that registered the artifact.
  RunId run;
  /// Logical tool call that published it, when applicable.
  std::optional<ToolCallId> call;
  /// Current publication availability.
  ArtifactAvailability availability;
  /// Selectable parts of the published version.
  std::vector<ArtifactPart> parts;
  /// Versioned input-to-output dependencies used in impact queries.
  std::vector<ArtifactDependency> dependencies;
  /// Original command associated with a source change, when recorded.
  std::optional<CommandId> source_change;
};

/// @brief Authorized display data for an exact version, returned by preview services.
struct ArtifactPreview {
  /// Version to which the display data belongs.
  ArtifactRef artifact;
  /// Host-defined structured content for a registered renderer, when available.
  std::optional<JsonValue> structured;
  /// Optional thumbnail reference supplied by the host preview adapter. The default UI does not render it.
  std::optional<ArtifactRef> thumbnail;
  /// Human-readable fallback or preview text.
  std::string summary;
  /// Artifact type used for rendering and actions.
  std::string type;
  /// Access projection revision bound to the returned preview.
  Revision access_revision = 0;
  /// Display name of the corresponding version.
  std::string name;
};

/// @brief Information for a host resolver after runtime authorization.
struct ArtifactResolveRequest {
  /// User or agent requesting access.
  ActionParty actor;
  /// Exact version to resolve.
  ArtifactRef artifact;
  /// Opaque host handle recorded when publishing this version.
  std::string storage_handle;
};

/// @brief Request for canonical text used by SourceSlice resolution.
struct ArtifactSelectionRequest {
  /// Authorized artifact version and storage handle.
  ArtifactResolveRequest resource;
  /// Part IDs in requested order. Empty requests the entire version's selectable text.
  std::vector<PartId> parts;
  /// Maximum bytes of canonical text to return.
  std::uint64_t byte_limit;
};

/// @brief Optional host callbacks for artifact display, reads and attachment staging.
struct ArtifactStore {
  /// Returns display data for the requested version.
  std::function<Async<ArtifactPreview>(ArtifactResolveRequest)> preview;
  /// Opens the requested version's exact stored bytes.
  std::function<Async<huxerui::AsyncInputStream>(ArtifactResolveRequest)> open;
  /// Returns canonical UTF-8 text for the requested parts, subject to the byte limit.
  std::function<Async<std::string>(ArtifactSelectionRequest)> selection;
  /// Resolves a staged upload owned by the actor for acceptance into the session.
  std::function<Async<ArtifactDraft>(ActionParty, SessionId, ArtifactId)> resolve_staged_attachment;
};

/// @brief Reason a recorded input version stopped being current.
enum class SourceChangeKind {
  /// The source was changed or superseded.
  Modified,
  /// The source was withdrawn.
  Withdrawn,
  /// A previous effect was reversed.
  Reverted
};

/// @brief Change to a versioned input used by downstream artifacts.
struct SourceChange {
  /// Original source and affected parts or range.
  SourceSlice before;
  /// Replacement version, when one exists and is visible.
  std::optional<SourceRef> after;
  /// Type of source transition.
  SourceChangeKind kind;
  /// Compares the stored fields for equality.
  bool operator==(const SourceChange&) const = default;
};

/// @brief Requests dependency traversal for proposed or recorded source changes.
struct ArtifactImpactQuery {
  /// Project in which dependencies may be traversed.
  ProjectId project;
  /// Explicit changes used for an exploratory impact query.
  std::vector<SourceChange> changes;
  /// Previously recorded source-change command to inspect, when supplied.
  std::optional<CommandId> change_command;
};

/// @brief One affected artifact and the visible path relating it to changed inputs.
struct ImpactItem {
  /// Downstream artifact version.
  ArtifactRef affected;
  /// Affected output parts when the dependency is known at that granularity.
  std::vector<PartId> parts;
  /// Ordered visible dependency slices connecting the change to the affected version.
  std::vector<SourceSlice> path;
  /// True when impact can only be reported at an approximate or whole-artifact scope.
  bool approximate;
};

/// @brief Permission-filtered impact query result.
struct ArtifactImpact {
  /// Project revision used for the query.
  Revision project_revision;
  /// Visible versions affected by the supplied changes.
  std::vector<ImpactItem> affected;
  /// Visible change resolved from change_command, when requested.
  std::optional<SourceChange> recorded_change;
};

/// @brief Uses one new source slice in place of an original dependency.
struct SourceReplacement {
  /// Original versioned dependency selection.
  SourceSlice previous;
  /// Replacement selection to bind into the new plan.
  SourceSlice replacement;
  /// Compares the stored fields for equality.
  bool operator==(const SourceReplacement&) const = default;
};

/// @brief Prepares regeneration or editing of an artifact against explicit versioned inputs.
struct ReworkRequest {
  /// Session to own the rework Run and approval.
  SessionId session;
  /// Exact artifact version to rework.
  ArtifactRef baseline;
  /// Requested output parts. Empty requests whole-artifact rework.
  std::vector<PartId> parts;
  /// Resource baselines to recheck before execution.
  std::vector<TargetBaseline> targets;
  /// Explicit old-to-new dependency substitutions.
  std::vector<SourceReplacement> replacements;
  /// Explicit consent to the larger scope when the adapter requires whole-artifact rework.
  bool accept_whole_artifact_scope = false;
  /// Compares the stored fields for equality.
  bool operator==(const ReworkRequest&) const = default;
};

/// @brief Prepares a compensating operation for a previously issued call.
struct RevertRequest {
  /// Session to own the revert operation.
  SessionId session;
  /// Logical call whose known effects are being reverted.
  ToolCallId original_call;
  /// Target versions the caller expects before the revert.
  std::vector<TargetBaseline> expected_current;
  /// Compares the stored fields for equality.
  bool operator==(const RevertRequest&) const = default;
};

/// @brief Prepares adoption from a comparison into a target session.
struct AdoptCandidateRequest {
  /// Comparison that owns the candidate.
  ForkReviewId review;
  /// Comparison revision expected by the caller.
  Revision expected_review;
  /// Candidate chosen for adoption.
  CandidateId candidate;
  /// Exact candidate artifact version to apply.
  ArtifactRef candidate_artifact;
  /// Session receiving the approved change.
  SessionId target_session;
  /// Selected artifact parts, when adopting a subset.
  std::vector<PartId> parts;
  /// Expected target resource baselines.
  std::vector<TargetBaseline> targets;
  /// Compares the stored fields for equality.
  bool operator==(const AdoptCandidateRequest&) const = default;
};

/// @brief Authorized inputs passed to a change adapter before any approved write.
struct ChangePrepareContext {
  /// Tool preparation identity, resources and cancellation.
  ToolPrepareContext access;
  /// Available artifact record associated with the operation, when applicable.
  std::optional<ArtifactRecord> artifact;
  /// Known original effects for compensation.
  std::vector<ResourceEffect> original_effects;
};

/// @brief Optional paired host callbacks for artifact changes.
/// Configure both the prepare and execute callback for each supported operation.
struct ChangeAdapters {
  /// Builds a reviewable plan for ReworkRequest.
  std::function<Async<PreparedChange>(ChangePrepareContext, ReworkRequest)> prepare_rework;
  /// Executes the approved rework plan.
  ToolHandler execute_rework;
  /// Builds a compensating plan from the original known effects.
  std::function<Async<PreparedChange>(ChangePrepareContext, RevertRequest)> prepare_revert;
  /// Executes the approved revert plan.
  ToolHandler execute_revert;
  /// Builds a reviewable plan for applying a candidate version.
  std::function<Async<PreparedChange>(ChangePrepareContext, AdoptCandidateRequest)> prepare_adoption;
  /// Executes the approved adoption plan.
  ToolHandler execute_adoption;
};

/// @brief User- or Run-bound artifact publication and change service.
class ArtifactChanges {
public:
  /// @brief Reads authorized publication metadata and provenance for an exact version.
  /// @param artifact Exact version to resolve under this service's actor.
  /// @return The visible metadata record for that version.
  Async<ArtifactRecord> ReadAsync(ArtifactRef artifact);

  /// @brief Resolves authorized display data for the exact artifact version.
  /// @param artifact Exact version to resolve.
  /// @return Display data authorized for the actor's current access revision.
  Async<ArtifactPreview> PreviewAsync(ArtifactRef artifact);

  /// @brief Opens an authorized stream for the exact version.
  /// @param artifact Exact version whose bytes are requested.
  /// @return The host stream for the authorized version.
  Async<huxerui::AsyncInputStream> OpenAsync(ArtifactRef artifact);

  /// @brief Traverses visible direct and transitive dependencies for the supplied source changes.
  /// @param query Project and proposed changes or the command for a previously recorded change.
  /// @return The visible dependencies and affected records.
  Async<ArtifactImpact> InspectImpactAsync(ArtifactImpactQuery query);

  /// @brief Records a project source change under a replayable command ID.
  /// @param project Scope that owns the source and its affected records.
  /// @param command Stable ID of this source change.
  /// @param change Baseline and resulting source versions with optional evidence.
  /// @return Receipt for the committed change record.
  Async<CommandReceipt> RecordSourceChangeAsync(ProjectId project, CommandId command, SourceChange change);

  /// @brief Accepts a rework request and returns a receipt for its Run, queue entry or approval.
  /// @param command Stable ID for this plan request.
  /// @param request Exact version to rework and the requested source replacements.
  /// @return Plan receipt and any required approval. Observe command progress for the final effect.
  Async<Receipt<ChangePlanAccepted>> PrepareReworkAsync(CommandId command, ReworkRequest request);

  /// @brief Accepts a compensating change and routes its plan through the applicable tool approval.
  /// @param command Stable ID for this plan request.
  /// @param request Exact change to reverse, with its expected baseline.
  /// @return Plan receipt and any required approval. Observe command progress for the final effect.
  Async<Receipt<ChangePlanAccepted>> PrepareRevertAsync(CommandId command, RevertRequest request);

private:
  explicit ArtifactChanges(std::shared_ptr<detail::RuntimeState> state, std::optional<RunId> actor = {});
  std::shared_ptr<detail::RuntimeState> state_;
  std::optional<RunId> actor_;
  friend class AgentRuntime;
  friend class RunContext;
};

} // namespace huxerui::agentkit
