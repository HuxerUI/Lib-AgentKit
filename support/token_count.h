#pragma once

#include <cstdint>
#include <string>

namespace huxerui::agentkit::detail {

inline std::string FormatTokenCount(std::uint64_t count, bool exact = false) {
  if (count < 1'000) return std::to_string(count);
  const bool millions = count >= 1'000'000;
  const std::uint64_t scale = millions ? 1'000'000 : 1'000;
  std::string result = std::to_string(count / scale);
  auto fraction = std::to_string(scale + count % scale).substr(1);
  if (!exact) fraction.resize(2);
  while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
  if (!fraction.empty()) result += "." + fraction;
  result += millions ? 'm' : 'k';
  return result;
}

} // namespace huxerui::agentkit::detail
