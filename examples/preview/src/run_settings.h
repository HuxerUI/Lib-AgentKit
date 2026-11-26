#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/file.h>
#include <huxerui/http.h>
#include <huxerui/presentation.h>

#include <set>

namespace agentkit_preview {

inline const huxerui::agentkit::PrincipalId PreviewUser{"preview.user"};
inline const huxerui::agentkit::ModelProviderRef DemoModel{{"preview.local"}, 1};
inline const huxerui::agentkit::ExecutionProfileRef AskProfile{{"preview.ask"}, 1};
inline const huxerui::agentkit::ExecutionProfileRef ReviewProfile{{"preview.auto-review"}, 1};

struct ComposerDraft {
  huxerui::State<huxerui::TextEditingValue> text{huxerui::TextEditingValue::FromText({})};
  huxerui::State<huxerui::agentkit::AgentChatDraftSources> sources{huxerui::agentkit::AgentChatDraftSources{}};
  huxerui::State<huxerui::agentkit::UserRunOptions> next{huxerui::agentkit::UserRunOptions{.execution_profile = AskProfile}};
  huxerui::State<std::vector<huxerui::agentkit::SkillRef>> skills{std::vector<huxerui::agentkit::SkillRef>{}};
  huxerui::State<std::vector<huxerui::agentkit::QueuedDraftReference>> recovered{
      std::vector<huxerui::agentkit::QueuedDraftReference>{}};
  huxerui::State<std::string> preference_error{std::string{}};
  bool saving_preference = false;
};

using ModelLimits = huxerui::agentkit::ModelContextLimits;

enum class ModelStrength { Unknown, Typical, Strong };

struct ModelExpertPreferences {
  ModelStrength code = ModelStrength::Unknown;
  ModelStrength reasoning = ModelStrength::Unknown;
  ModelStrength vision = ModelStrength::Unknown;
  bool automatic = false;

  bool operator==(const ModelExpertPreferences&) const = default;
};

struct ModelChoice {
  huxerui::agentkit::ModelProviderRef ref;
  std::string label;
  std::string name;
  bool code;
  bool suitable;
  huxerui::agentkit::ModelCapabilities capabilities;
  std::string endpoint;
  huxerui::agentkit::ModelEndpoint connection;
  ModelLimits limits;
  bool editable = false;
  ModelExpertPreferences experts;
};

struct ModelProfileInput {
  std::string model;
  std::string base_url;
  std::string protocol;
  std::string provider;
  std::string api_key;
  bool image_input = false;
  bool file_input = false;
  bool reasoning_summary = false;
  ModelLimits limits;
  ModelExpertPreferences experts;
};

struct DiscoveredModel {
  std::string id;
  std::string name;
  ModelLimits limits;
};

std::string TrimModelField(std::string value);
std::string ModelKeyEnvironment(std::string_view key);
huxerui::agentkit::Async<std::vector<DiscoveredModel>> DiscoverModels(
    ModelProfileInput endpoint, std::shared_ptr<huxerui::HttpClient> client);

huxerui::agentkit::Async<void> TestModelConnection(ModelProfileInput profile);

bool SupportsImageInput(std::string_view protocol);

using AddModelProfile = std::function<huxerui::agentkit::Async<huxerui::agentkit::ModelProviderRef>(ModelProfileInput)>;

using EditModelProfile = std::function<huxerui::agentkit::Async<huxerui::agentkit::ModelProviderRef>(
    huxerui::agentkit::ModelProviderRef, std::string endpoint, std::optional<std::string> new_key, ModelLimits,
    bool image_input, ModelExpertPreferences)>;
using RemoveModelProfile = std::function<huxerui::agentkit::Async<void>(huxerui::agentkit::ModelProviderRef)>;

void ShowAddModel(const huxerui::DialogHandle& dialog, AddModelProfile add_model,
    std::function<void(huxerui::agentkit::ModelProviderRef)> added);

void ShowEditModel(const huxerui::DialogHandle& dialog, ModelChoice current, EditModelProfile edit_model);

class RunSettings {
public:
  void AddModel(const huxerui::agentkit::ModelRegistration& registration, std::string label = {},
      ModelLimits limits = {}, bool editable = false, ModelExpertPreferences experts = {});
  void RetireModel(const huxerui::agentkit::ModelId& id);
  bool IsRetired(const huxerui::agentkit::ModelId& id) const;
  bool HasRetiredDefault(const huxerui::agentkit::AgentId& agent) const;
  void SetDefaults(huxerui::agentkit::ModelProviderRef general, std::optional<huxerui::agentkit::ModelProviderRef> code);
  std::optional<huxerui::agentkit::ModelProviderRef> DefaultModel(const huxerui::agentkit::AgentId& agent) const;
  std::optional<huxerui::agentkit::ModelProviderRef> CurrentModel(
      std::optional<huxerui::agentkit::ModelProviderRef> model) const;
  std::vector<ModelChoice> Models(const huxerui::agentkit::AgentId& agent) const;
  std::optional<huxerui::agentkit::ModelProviderRef> SelectExpertModel(const huxerui::agentkit::AgentId& expert,
      const std::optional<huxerui::agentkit::ModelProviderRef>& preferred, const huxerui::agentkit::ModelProviderRef& parent,
      huxerui::agentkit::ModelCapabilities required = {}, std::size_t task_bytes = 0) const;
  huxerui::StringVariant ModelLabel(const std::optional<huxerui::agentkit::ModelProviderRef>& ref) const;
  std::optional<huxerui::Image> ModelAvatar(const huxerui::agentkit::ModelProviderRef& ref, huxerui::Color foreground) const;
  huxerui::agentkit::ModelCapabilities Capabilities(const std::optional<huxerui::agentkit::ModelProviderRef>& ref,
      const huxerui::agentkit::AgentId& agent) const;
  std::string Describe(const huxerui::agentkit::RunStartBinding& binding, const huxerui::Resources& resources) const;
  bool HasFullAccess(const huxerui::agentkit::ExecutionProfileRef& profile,
                    const std::optional<huxerui::agentkit::WorkspaceId>& workspace) const;

  huxerui::agentkit::ExecutionProfileRef GrantFullAccess(huxerui::agentkit::WorkspaceId workspace);
  huxerui::agentkit::AgentResult<huxerui::agentkit::PolicyDecision> Decide(
      const huxerui::agentkit::PolicyRequest& request, bool needs_confirmation, bool workspace_available, bool full_local_access) const;

private:
  std::vector<ModelChoice> models_;
  std::map<huxerui::agentkit::ModelId, huxerui::agentkit::Revision> model_versions_;
  std::set<huxerui::agentkit::ModelId> retired_models_;
  huxerui::State<std::size_t> model_revision_{std::size_t{0}};
  huxerui::agentkit::ModelProviderRef general_ = DemoModel;
  std::optional<huxerui::agentkit::ModelProviderRef> code_;
  std::map<huxerui::agentkit::ExecutionProfileId, huxerui::agentkit::WorkspaceId, std::less<>> full_access_;
};

huxerui::agentkit::ToolReviewer ChartExportReviewer(huxerui::File export_folder,
    std::optional<huxerui::agentkit::AgentResult<huxerui::agentkit::ModelRegistration>> model = {});
huxerui::View RunSettingsControls(std::shared_ptr<RunSettings> settings,
    huxerui::State<huxerui::agentkit::UserRunOptions> next, huxerui::agentkit::AgentId agent,
    std::optional<huxerui::agentkit::WorkspaceId> workspace, std::optional<huxerui::File> workspace_root,
    std::function<void()> changed = {},
    std::optional<huxerui::View> extra_action = {}, AddModelProfile add_model = {}, bool show_permissions = true,
    bool full_local_access = false, huxerui::agentkit::ExecutionProfileRef default_profile = AskProfile);

} // namespace agentkit_preview
