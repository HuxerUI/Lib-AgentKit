#pragma once

#include "run_settings.h"

#include <huxerui/theme.h>

namespace agentkit_preview {

enum class PreviewAppearance { Dark, Light, System };
enum class ReplyTone { Default, Concise, Friendly, Formal };

struct PreviewSettings {
  PreviewAppearance appearance = PreviewAppearance::Dark;
  ReplyTone tone = ReplyTone::Default;
  std::string instructions;
  std::optional<huxerui::agentkit::ModelProviderRef> daily_model;
  std::optional<huxerui::agentkit::ModelProviderRef> image_model;
  std::optional<huxerui::agentkit::ModelProviderRef> review_model;

  bool operator==(const PreviewSettings&) const = default;
};

huxerui::agentkit::Async<PreviewSettings> ReadPreviewSettings(huxerui::File file);
huxerui::agentkit::Async<void> SavePreviewSettings(huxerui::File file, PreviewSettings settings);
std::string WithReplyPreferences(std::string instructions, const PreviewSettings& settings);
huxerui::ThemeBrightness PreviewBrightness(PreviewAppearance appearance, huxerui::ThemeBrightness system);

huxerui::View PreviewSettingsPage(huxerui::State<PreviewSettings> current, huxerui::State<PreviewSettings> applied,
    std::shared_ptr<RunSettings> models, huxerui::File file, AddModelProfile add_model, EditModelProfile edit_model, std::function<void()> close);

} // namespace agentkit_preview
