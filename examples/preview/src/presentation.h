#pragma once

#include <huxerui/agentkit/types.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <stop_token>

namespace agentkit_preview {

struct SlideOutline {
  std::string title;
  std::vector<std::string> bullets;
  bool operator==(const SlideOutline&) const = default;
};

struct PresentationOutline {
  std::string title;
  std::vector<SlideOutline> slides;
};

huxerui::agentkit::JsonSchema PresentationOutlineSchema();
huxerui::agentkit::AgentResult<PresentationOutline> ReadPresentationOutline(const huxerui::agentkit::JsonValue& value);
huxerui::agentkit::JsonValue WritePresentationOutline(const PresentationOutline& outline);
huxerui::agentkit::AgentResult<void> CreatePptx(const PresentationOutline& outline, const huxerui::File& destination,
    const std::optional<huxerui::ImageAsset>& screenshot = {});

struct PptxTextChange {
  std::size_t slide;
  std::string previous;
  std::string replacement;
};

huxerui::agentkit::AgentResult<void> ReworkPptxText(const huxerui::File& source,
    const std::optional<huxerui::File>& destination, const std::vector<PptxTextChange>& changes, std::stop_token stop);

} // namespace agentkit_preview
