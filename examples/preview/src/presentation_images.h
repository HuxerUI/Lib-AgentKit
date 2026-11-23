#pragma once

#include <huxerui/agentkit/runtime.h>
#include <huxerui/resource.h>

namespace agentkit_preview {

struct PresentationImageModel {
  std::string base_url;
  std::string api_key;
  std::string model;
};

using PresentationImageProvider = std::function<huxerui::agentkit::Async<PresentationImageModel>()>;

huxerui::agentkit::Async<huxerui::ImageAsset> GeneratePresentationImage(const PresentationImageModel& model,
    std::string prompt, bool portrait, std::stop_token stop);

} // namespace agentkit_preview
