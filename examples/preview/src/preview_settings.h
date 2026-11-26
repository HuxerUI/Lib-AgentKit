#pragma once

#include "run_settings.h"
#include "local_files.h"
#include "imported_skills.h"
#include "mcp_connections.h"

#include <huxerui/theme.h>

namespace agentkit_preview {

enum class PreviewAppearance { Dark, Light, System };
enum class PreviewPalette;
enum class ReplyTone { Default, Concise, Friendly, Formal };

struct PreviewSettings {
  PreviewAppearance appearance = PreviewAppearance::Dark;
  PreviewPalette palette{};
  ReplyTone tone = ReplyTone::Default;
  std::string instructions;
  std::optional<huxerui::agentkit::ModelProviderRef> daily_model;
  std::optional<huxerui::agentkit::ModelProviderRef> image_model;
  std::optional<huxerui::agentkit::ModelProviderRef> review_model;
  std::optional<huxerui::agentkit::ModelProviderRef> image_generation_model;
  LocalFilePermissions file_access;
  std::vector<McpServer> mcp_servers;

  bool operator==(const PreviewSettings&) const = default;
};

huxerui::agentkit::Async<PreviewSettings> ReadPreviewSettings(huxerui::File file);
huxerui::agentkit::Async<void> SavePreviewSettings(huxerui::File file, PreviewSettings settings);
void ClearRetiredModelRoles(PreviewSettings& settings, const RunSettings& models);
std::string WithReplyPreferences(std::string instructions, const PreviewSettings& settings);
huxerui::ThemeSpec PreviewThemeFor(const PreviewSettings& settings, huxerui::ThemeBrightness system);

huxerui::View PreviewSettingsPage(huxerui::State<PreviewSettings> current, huxerui::State<PreviewSettings> applied,
    std::shared_ptr<RunSettings> models, huxerui::File file, AddModelProfile add_model, EditModelProfile edit_model,
    RemoveModelProfile remove_model,
    std::function<void()> close, std::function<huxerui::agentkit::Async<void>(LocalFilePermissions)> apply_file_access,
    std::shared_ptr<ImportedSkillSource> imported_skills, std::shared_ptr<McpConnections> mcp,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::State<bool> busy);

} // namespace agentkit_preview
