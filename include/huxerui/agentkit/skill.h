#pragma once

/// @file
/// @brief Versioned skill discovery, on-demand activation and resource access.
/// Register a SkillSource through AgentRuntime::RegisterSkillSource() and list its ID in AgentDefinition::skill_sources.
/// RunContext exposes active instructions and can read resources bound to that activation.

#include <huxerui/agentkit/session.h>
#include <huxerui/stream.h>

#include <stop_token>

namespace huxerui::agentkit {

/// @brief Built-in tool ID for activating an offered SkillRef.
inline const ToolId SkillActivationTool{"agentkit_activate_skill"};
/// @brief Built-in tool ID for reading a textual resource from an active skill.
inline const ToolId SkillResourceTool{"agentkit_read_skill"};

/// @brief Selects the authorization context for listing skill summaries.
struct SkillCatalogQuery {
  /// A registered AgentId or an existing SessionId.
  std::variant<AgentId, SessionId> target;
  /// Project scope for an agent-based query. A session-based query uses its session project.
  std::optional<ProjectId> project;
  /// Workspace scope for an agent-based query. A session-based query uses its session workspace.
  std::optional<WorkspaceId> workspace;
};

/// @brief Identity and scope passed to a SkillSource by AgentRuntime.
struct SkillSourceRequest {
  /// User or running agent on whose behalf the source is read.
  ActionParty actor;
  /// Versioned definition for which the skill is being resolved.
  AgentRef agent;
  /// Project scope, when the runtime has a project binding.
  std::optional<ProjectId> project;
  /// Workspace scope, when bound.
  std::optional<WorkspaceId> workspace;
  /// Cancellation signal for the current discovery or read.
  std::stop_token stop;
};

/// @brief Resource manifest entry in a SkillPackage.
struct SkillResource {
  /// Normalized path relative to the package root.
  std::string relative_path;
  /// Media type used for resource handling and eligibility for model text reads.
  std::string media_type;
  /// Known byte size for budget checks, when available.
  std::optional<std::uint64_t> size_bytes;
};

/// @brief The body and resource manifest of an exact SkillRef.
struct SkillPackage {
  /// Source, ID and version returned by discovery.
  SkillRef ref;
  /// UTF-8 Markdown instructions for the active skill.
  std::string instructions;
  /// Resources that may be opened for this version.
  std::vector<SkillResource> resources;
};

/// @brief Host implementation for a registered, versioned skill catalog.
/// ListAsync() returns metadata. LoadAsync() and OpenResourceAsync() run after their respective permission checks.
/// A source must resolve the requested version or report it unavailable, including during Run recovery.
class SkillSource {
public:
  /// @brief Releases the skill source implementation.
  virtual ~SkillSource() = default;

  /// @brief Lists summaries visible in the supplied context.
  /// @param request Caller identity, scope and cancellation for discovery.
  /// @return Skill versions visible in the requested scope.
  virtual Async<std::vector<SkillSummary>> ListAsync(SkillSourceRequest request) = 0;

  /// @brief Loads instructions and the manifest for the exact discovered SkillRef.
  /// @param request Caller identity, scope and cancellation for the load.
  /// @param skill Versioned skill from this source.
  /// @return The manifest and instructions for that exact skill package.
  virtual Async<SkillPackage> LoadAsync(SkillSourceRequest request, SkillRef skill) = 0;

  /// @brief Opens an owned stream for a resource in the exact SkillRef.
  /// @param request Caller identity, scope and cancellation for the read.
  /// @param skill Active package version whose manifest lists the resource.
  /// @param relative_path The requested normalized package-relative path.
  /// @return The stream for the requested authorized manifest resource.
  virtual Async<huxerui::AsyncInputStream> OpenResourceAsync(
      SkillSourceRequest request, SkillRef skill, std::string relative_path) = 0;
};

} // namespace huxerui::agentkit
