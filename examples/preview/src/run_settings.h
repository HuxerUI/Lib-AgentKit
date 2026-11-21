#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/file.h>
#include <huxerui/presentation.h>

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

struct ModelChoice {
  huxerui::agentkit::ModelProviderRef ref;
  std::string label;
  std::string name;
  bool code;
  bool suitable;
  huxerui::agentkit::ModelCapabilities capabilities;
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
};

using AddModelProfile = std::function<huxerui::agentkit::Async<huxerui::agentkit::ModelProviderRef>(ModelProfileInput)>;

void ShowAddModel(const huxerui::DialogHandle& dialog, AddModelProfile add_model,
    std::function<void(huxerui::agentkit::ModelProviderRef)> added);

class RunSettings {
public:
  explicit RunSettings(std::function<huxerui::agentkit::TimePoint()> clock = {});

  void AddModel(const huxerui::agentkit::ModelRegistration& registration, std::string label = {});
  void SetDefaults(huxerui::agentkit::ModelProviderRef general, std::optional<huxerui::agentkit::ModelProviderRef> code);
  std::optional<huxerui::agentkit::ModelProviderRef> DefaultModel(const huxerui::agentkit::AgentId& agent) const;
  std::vector<ModelChoice> Models(const huxerui::agentkit::AgentId& agent) const;
  huxerui::StringVariant ModelLabel(const std::optional<huxerui::agentkit::ModelProviderRef>& ref) const;
  huxerui::agentkit::ModelCapabilities Capabilities(const std::optional<huxerui::agentkit::ModelProviderRef>& ref,
      const huxerui::agentkit::AgentId& agent) const;
  std::string Describe(const huxerui::agentkit::RunStartBinding& binding, const huxerui::Resources& resources) const;
  bool NeedsRenewal(const huxerui::agentkit::ExecutionProfileRef& profile,
                    const std::optional<huxerui::agentkit::WorkspaceId>& workspace) const;

  huxerui::agentkit::ExecutionProfileRef GrantFullAccess(huxerui::agentkit::WorkspaceId workspace);
  huxerui::agentkit::AgentResult<huxerui::agentkit::PolicyDecision> Decide(
      const huxerui::agentkit::PolicyRequest& request, bool needs_confirmation, bool workspace_available) const;

private:
  struct FullAccessGrant {
    huxerui::agentkit::WorkspaceId workspace;
    huxerui::agentkit::TimePoint expires;
  };
  std::function<huxerui::agentkit::TimePoint()> clock_;
  std::vector<ModelChoice> models_;
  huxerui::agentkit::ModelProviderRef general_ = DemoModel;
  std::optional<huxerui::agentkit::ModelProviderRef> code_;
  std::map<huxerui::agentkit::ExecutionProfileId, FullAccessGrant, std::less<>> full_access_;
};

huxerui::agentkit::ToolReviewer ChartExportReviewer(huxerui::File export_folder,
    std::optional<huxerui::agentkit::AgentResult<huxerui::agentkit::ModelRegistration>> model = {});
huxerui::View RunSettingsControls(std::shared_ptr<RunSettings> settings,
    huxerui::State<huxerui::agentkit::UserRunOptions> next, huxerui::agentkit::AgentId agent,
    std::optional<huxerui::agentkit::WorkspaceId> workspace, std::optional<huxerui::File> workspace_root,
    std::optional<huxerui::agentkit::RunStartBinding> current = {}, std::function<void()> changed = {},
    std::optional<huxerui::View> extra_action = {}, AddModelProfile add_model = {});

} // namespace agentkit_preview
