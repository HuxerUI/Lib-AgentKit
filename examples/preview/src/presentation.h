#pragma once

#include <huxerui/agentkit/types.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <map>
#include <span>
#include <stop_token>

namespace agentkit_preview {

inline constexpr int PresentationWidth = 12192000;
inline constexpr int PresentationHeight = 6858000;

enum class SlideLayout { Cover, List, Cards, Flow, Comparison, Image, Chart, Relationship, Editorial };

struct PresentationChart {
  std::string unit;
  std::string source;
  std::string url;
  std::vector<std::pair<std::string, double>> points;
  bool operator==(const PresentationChart&) const = default;
};

struct PresentationChartAxis {
  double minimum;
  double maximum;
  double step;
};

PresentationChartAxis ChartAxis(const PresentationChart& chart);

struct SlideOutline {
  std::string title;
  std::vector<std::string> bullets;
  std::optional<SlideLayout> layout;
  std::optional<std::string> media;
  bool operator==(const SlideOutline&) const = default;
};

struct PresentationOutline {
  std::string title;
  std::vector<SlideOutline> slides;
  std::optional<PresentationChart> supplied_data;
};

enum class SlideLayoutField { Omit, Optional, Visual };

inline constexpr std::string_view OctoverseData = "github-octoverse-2025";
inline constexpr std::string_view UserChartData = "user-provided-data";

std::optional<PresentationChart> ReadSuppliedChart(std::string_view text);
const PresentationChart* ChartFor(const PresentationOutline& outline, const SlideOutline& slide);
std::string DescribePresentationData(const std::optional<PresentationChart>& supplied);

struct SlideRect { int x, y, width, height; };

enum class SlideShape { Text, Rectangle, Circle, RightArrow, LeftArrow, UpArrow, DownArrow };

struct SlideElement {
  SlideRect bounds;
  SlideShape shape;
  unsigned color;
  std::string text;
  int size = 0;
  bool bold = false;
  bool centered = false;
  std::optional<std::size_t> excerpt;
};

struct SlideScene {
  unsigned background;
  std::vector<SlideElement> elements;
  std::optional<SlideRect> image;
  std::optional<SlideRect> chart;
};

inline constexpr bool UsesSlideScene(SlideLayout layout, const SlideOutline& slide) {
  return layout == SlideLayout::Image || layout == SlideLayout::Chart || layout == SlideLayout::Relationship ||
      layout == SlideLayout::Editorial || (layout == SlideLayout::Flow && slide.media.has_value());
}

SlideScene ComposeSlide(const PresentationOutline& outline, std::size_t page);

huxerui::agentkit::JsonSchema PresentationOutlineSchema(SlideLayoutField layout = SlideLayoutField::Omit, bool include_evidence = false);
huxerui::agentkit::AgentResult<PresentationOutline> ReadPresentationOutline(const huxerui::agentkit::JsonValue& value);
huxerui::agentkit::JsonValue WritePresentationOutline(const PresentationOutline& outline);
SlideLayout ResolveSlideLayout(const PresentationOutline& outline, std::size_t page, bool has_screenshot = false);
huxerui::agentkit::AgentResult<void> CreatePptx(const PresentationOutline& outline, const huxerui::File& destination,
    const std::optional<huxerui::ImageAsset>& screenshot = {},
    const std::map<std::size_t, huxerui::ImageAsset>& illustrations = {});
huxerui::agentkit::AgentResult<PresentationOutline> ReadPackedPresentationPlan(std::span<const std::byte> bytes);

struct PptxTextChange {
  std::size_t slide;
  std::string previous;
  std::string replacement;
};

huxerui::agentkit::AgentResult<void> ReworkPptxText(const huxerui::File& source,
    const std::optional<huxerui::File>& destination, const std::vector<PptxTextChange>& changes, std::stop_token stop);

} // namespace agentkit_preview
