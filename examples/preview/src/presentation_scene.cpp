#include "presentation.h"

#include <huxerui/data.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace agentkit_preview {
namespace {

const PresentationChart octoverse{
    "GitHub contributions (100 million)", "GitHub Octoverse 2025",
    "https://github.blog/news-insights/octoverse/octoverse-a-new-developer-joins-github-every-second-as-ai-leads-typescript-to-1/",
    {{"2024 (~)", 10.0}, {"2025", 11.28}}};

std::string_view Trim(std::string_view line) {
  const auto start = line.find_first_not_of(" \t\r");
  if (start == line.npos) return {};
  return line.substr(start, line.find_last_not_of(" \t\r") - start + 1);
}

std::optional<std::pair<std::string_view, std::string_view>> Cells(std::string_view row) {
  if (!row.starts_with('|') || !row.ends_with('|') || row.size() < 4) return {};
  row = row.substr(1, row.size() - 2);
  const auto divider = row.find('|');
  if (divider == row.npos || row.find('|', divider + 1) != row.npos) return {};
  const auto label = Trim(row.substr(0, divider));
  const auto value = Trim(row.substr(divider + 1));
  if (label.empty() || value.empty() || label.size() > 96 || value.size() > 96) return {};
  return std::pair{label, value};
}

bool Separator(std::string_view cell) {
  return cell.find('-') != cell.npos && cell.find_first_not_of(":-") == cell.npos;
}

float TextWidthInEm(std::string_view text) {
  float width = 0;
  for (unsigned char byte : text) {
    if (byte < 0x80) width += byte == ' ' ? 0.3F : 0.56F;
    else if (byte >= 0xC0) width += 1.0F;
  }
  return std::max(1.0F, width);
}

constexpr unsigned paper = 0xF5EFE3;
constexpr unsigned ink = 0x221F1B;
constexpr unsigned dark = 0x25211E;
constexpr unsigned pale = 0xFAF3E8;
constexpr unsigned muted = 0x655D54;
constexpr unsigned red = 0xB63A29;
constexpr unsigned line = 0xCCBFAF;

} // namespace

std::optional<PresentationChart> ReadSuppliedChart(std::string_view text) {
  std::istringstream stream{std::string(text)};
  std::string input;
  PresentationChart chart;
  while (std::getline(stream, input)) {
    const auto source = Trim(input);
    constexpr std::string_view english = "Source:";
    constexpr std::string_view chinese = "\xe6\x9d\xa5\xe6\xba\x90\xef\xbc\x9a";
    const auto prefix = source.starts_with(english) ? english : source.starts_with(chinese) ? chinese : std::string_view{};
    if (prefix.empty()) continue;
    const auto url = Trim(source.substr(prefix.size()));
    const auto uri = huxerui::Uri::Parse(url);
    if (!uri || uri->Scheme() != "https" || !uri->Authority() || uri->Authority()->empty() ||
        uri->Authority()->find('@') != std::string_view::npos || url.size() > 400) return {};
    chart.source = std::string(*uri->Authority());
    chart.url = url;
    break;
  }
  if (chart.url.empty()) return {};
  while (std::getline(stream, input)) {
    const auto header = Cells(Trim(input));
    if (!header) continue;
    chart.unit = header->second;
    if (!std::getline(stream, input)) return {};
    const auto separator = Cells(Trim(input));
    if (!separator || !Separator(separator->first) || !Separator(separator->second)) return {};
    while (std::getline(stream, input)) {
      const auto row = Trim(input);
      if (!row.starts_with('|')) break;
      const auto cells = Cells(row);
      if (!cells || chart.points.size() >= 8) return {};
      double value = 0.0;
      const auto numeric = cells->second;
      const auto parsed = std::from_chars(numeric.data(), numeric.data() + numeric.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != numeric.data() + numeric.size() || !std::isfinite(value) ||
          std::abs(value) > 1e12 || std::ranges::find(chart.points, cells->first, &std::pair<std::string, double>::first) != chart.points.end())
        return {};
      chart.points.emplace_back(cells->first, value);
    }
    if (chart.points.size() >= 2) return chart;
    return {};
  }
  return {};
}

const PresentationChart* ChartFor(const PresentationOutline& outline, const SlideOutline& slide) {
  if (slide.layout != SlideLayout::Chart || !slide.media) return nullptr;
  if (*slide.media == OctoverseData) return &octoverse;
  if (*slide.media == UserChartData && outline.supplied_data) return &*outline.supplied_data;
  return nullptr;
}

PresentationChartAxis ChartAxis(const PresentationChart& chart) {
  const auto [min_value, max_value] = std::ranges::minmax_element(chart.points, {},
      &std::pair<std::string, double>::second);
  const auto low = std::min(0.0, min_value->second);
  const auto high = std::max(0.0, max_value->second);
  const auto range = high - low;
  if (range < 1e-100) return {0, 1, 1};
  const auto magnitude = std::pow(10.0, std::floor(std::log10(range / 5)));
  const auto relative = range / (5 * magnitude);
  const auto step = magnitude * (relative <= 1 ? 1 : relative <= 2 ? 2 : relative <= 5 ? 5 : 10);
  return {std::floor(low / step) * step, std::ceil(high / step) * step, step};
}

std::string DescribePresentationData(const std::optional<PresentationChart>& supplied) {
  auto evidence = "\nAvailable chart datasets (media selects the dataset ID; use only when relevant):\n"
      "github-octoverse-2025: GitHub Octoverse 2025; contributions to GitHub public and open-source repositories. "
      "2024 approximately 10 hundred million; 2025 11.28 hundred million. Scope is GitHub, not all open source.\n";
  std::string result(evidence);
  if (supplied) {
    result += "user-provided-data: " + supplied->unit + ", from the user-supplied table attributed to " + supplied->url + ".\n";
    for (const auto& [name, value] : supplied->points) result += name + ": " + std::to_string(value) + "\n";
  }
  return result;
}

SlideScene ComposeSlide(const PresentationOutline& outline, std::size_t page) {
  const auto& slide = outline.slides[page];
  const auto layout = ResolveSlideLayout(outline, page);
  const bool embedded = slide.media && slide.media->starts_with("embedded");
  const bool cover = layout == SlideLayout::Image && (embedded ? *slide.media == "embedded-cover" : page == 0);
  const bool close = layout == SlideLayout::Image && (embedded ? *slide.media == "embedded-close" :
      page + 1 == outline.slides.size() && !cover);
  SlideScene scene{.background = layout == SlideLayout::Relationship ? dark : paper};
  const auto text = [&](SlideRect bounds, std::string value, int size, unsigned color, bool bold = false,
                        std::optional<std::size_t> excerpt = {}, bool centered = false) {
    scene.elements.push_back({bounds, SlideShape::Text, color, std::move(value), size, bold, centered, excerpt});
  };
  const auto shape = [&](SlideRect bounds, unsigned color, SlideShape kind = SlideShape::Rectangle) {
    scene.elements.push_back({bounds, kind, color});
  };
  const auto heading = [&](SlideRect bounds, int size, unsigned color = ink) {
    text(bounds, slide.title, size, color, true, 0);
  };
  const auto point = [&](std::size_t i, SlideRect bounds, int font, unsigned color = ink, bool bold = false) {
    text(bounds, slide.bullets[i], font, color, bold, i + 1);
  };
  const auto count = static_cast<int>(slide.bullets.size());
  if (layout == SlideLayout::Image) {
    if (cover || close) {
      scene.image = close ? SlideRect{640, 85, 640, 550} : SlideRect{666, 0, 614, 720};
      shape({76, 154, 46, 4}, red);
      const auto title_size = std::clamp(static_cast<int>(968 / TextWidthInEm(slide.title)), 38, 71);
      heading({70, 202, 540, 208}, title_size);
      const auto row = std::min(70, 232 / std::max(1, count));
      for (int i = 0; i < count; ++i) {
        const auto size = std::clamp(static_cast<int>(518 / TextWidthInEm(slide.bullets[i])), 20, 24);
        point(i, {76, 422 + i * row, 528, row - 6}, size, muted);
      }
    } else {
      scene.image = SlideRect{0, 0, 545, 720};
      heading({602, 81, 604, 115}, 53);
      const auto row = std::min(94, 440 / std::max(1, count));
      for (int i = 0; i < count; ++i) {
        shape({606, 228 + i * row, 592, 1}, line);
        point(i, {606, 246 + i * row, 590, row - 16}, 24);
      }
    }
  } else if (layout == SlideLayout::Chart) {
    heading({75, 65, 1125, 87}, 51);
    scene.chart = SlideRect{74, 178, 742, 410};
    shape({859, 224, 38, 4}, red);
    const auto row = std::min(115, 324 / std::max(1, count));
    for (int i = 0; i < count; ++i) point(i, {857, 262 + i * row, 325, row - 8}, i ? 23 : 28, i ? muted : ink, i == 0);
    if (const auto* chart = ChartFor(outline, slide)) {
      text({79, 621, 1110, 38}, chart->source + "  |  " + chart->unit, 19, 0x176E8C);
    }
  } else if (layout == SlideLayout::Relationship) {
    heading({76, 70, 1130, 100}, 51, pale);
    constexpr SlideRect cards[]{{158, 242, 365, 116}, {746, 242, 365, 116},
        {746, 480, 365, 116}, {158, 480, 365, 116}};
    constexpr unsigned coral = 0xEE6951;
    for (int i = 0; i < count && i < 4; ++i) {
      const auto& bounds = cards[i];
      shape({bounds.x - 24, bounds.y + 13, 11, 11}, coral, SlideShape::Circle);
      point(i, bounds, 29, pale, true);
    }
    shape({540, 257, 160, 13}, coral, SlideShape::RightArrow);
    shape({914, 365, 14, 72}, coral, SlideShape::DownArrow);
    shape({539, 496, 160, 13}, coral, SlideShape::LeftArrow);
    shape({323, 365, 14, 72}, coral, SlideShape::UpArrow);
  } else if (layout == SlideLayout::Flow) {
    heading({76, 70, 1128, 102}, 51);
    const int stride = 1114 / std::max(1, count);
    shape({107, 291, stride * std::max(0, count - 1), 2}, line);
    for (int i = 0; i < count; ++i) {
      const int x = 75 + i * stride;
      shape({x, 260, 64, 64}, i ? dark : red, SlideShape::Circle);
      text({x, 271, 64, 44}, std::to_string(i + 1), 29, pale, true, {}, true);
      point(i, {x, 373, stride - 18, 188}, 29, ink, true);
      if (i + 1 < count) shape({x + stride / 2, 285, 45, 13}, red, SlideShape::RightArrow);
    }
  } else {
    shape({0, 0, 466, 720}, dark);
    shape({69, 101, 41, 4}, red);
    heading({67, 163, 345, 370}, 56, pale);
    const auto row = std::min(130, 548 / std::max(1, count));
    for (int i = 0; i < count; ++i) {
      if (i) shape({523, 98 + i * row, 667, 1}, line);
      point(i, {526, 116 + i * row, 659, row - 15}, count > 4 ? 25 : 29);
    }
  }
  return scene;
}

} // namespace agentkit_preview
