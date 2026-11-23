#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

std::shared_ptr<huxerui::agentkit::AgentChatLinkHandler> MakePreviewLinkHandler();

bool CanOpenLocalPages();

bool OpenLocalPage(const huxerui::File& file);

} // namespace agentkit_preview
