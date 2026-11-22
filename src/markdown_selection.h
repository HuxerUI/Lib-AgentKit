#pragma once

#include <huxerui/markdown.h>

namespace huxerui::agentkit::detail {

#ifdef HUXERUI_MARKDOWN_SOURCE_MAPPING
inline constexpr bool HasMarkdownSourceSelection = true;
#else
inline constexpr bool HasMarkdownSourceSelection = false;
#endif

markdown::ParseOptions MarkdownSelectionParseOptions();

bool IsPlainMarkdown(const markdown::MarkdownDocument& document, std::string_view source);

struct MarkdownPosition {
  std::size_t paragraph;
  TextOffset offset;
  auto operator<=>(const MarkdownPosition&) const = default;
};

struct MarkdownRange {
  MarkdownPosition anchor;
  MarkdownPosition active;
  bool operator==(const MarkdownRange&) const = default;
};

struct MarkdownSourceExcerpt {
  std::string text;
  std::vector<std::pair<std::size_t, std::size_t>> source_bytes;
};

struct MarkdownExcerpt {
  MarkdownRange range;
  MarkdownSourceExcerpt source;
  std::optional<Rect> active_caret;
  bool pointer_selecting = false;
};

std::optional<MarkdownSourceExcerpt> ReadMarkdownSourceRange(std::string_view source, std::size_t begin, std::size_t end);

using MarkdownImageLoad = std::function<Task<ImageAsset>(std::string)>;

struct ChatReadingPosition {
  std::string owner;
  std::size_t source_byte = 0;

  bool operator==(const ChatReadingPosition&) const = default;
};

struct ChatReadingPositionKey {
  using Value = ChatReadingPosition;
};

View SelectableMarkdown(markdown::MarkdownDocument document, markdown::MarkdownOptions options, Color highlight,
                        ViewHandle focus, std::optional<MarkdownRange> selection,
                        std::function<void(std::optional<MarkdownExcerpt>)> changed,
                        std::function<void(const markdown::MarkdownLinkActivation&)> activate, MarkdownImageLoad load,
                        std::string reading_owner);

View SelectablePlainText(std::string text, TextStyle style, TextShapingOptions shaping, Color highlight,
                         ViewHandle focus, std::optional<MarkdownRange> selection,
                         std::function<void(std::optional<MarkdownExcerpt>)> changed);

std::optional<std::size_t> Utf8Offset(std::string_view text, TextOffset units);

} // namespace huxerui::agentkit::detail
