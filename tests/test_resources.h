#pragma once

#include <huxerui/huxerui.h>

#include <stdexcept>

namespace agentkit_test {

class PackagedResources final : public huxerui::PlatformResources {
public:
  huxerui::ResourceConfiguration Configuration() const override { return {}; }

  std::optional<huxerui::InputStream> OpenRead(std::string_view path) override {
    auto stream = package_.Resolve(path).OpenRead();
    if (stream.Succeeded()) return std::move(stream).Value();
    if (stream.Error().code == huxerui::IoErrorCode::NotFound) return {};
    throw std::runtime_error(stream.Error().message);
  }

private:
  huxerui::File package_{AGENTKIT_TEST_RESOURCE_PACKAGE};
};

} // namespace agentkit_test
