#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/file.h>
#include <map>

namespace agentkit_preview {

class AttachmentImports {
public:
  explicit AttachmentImports(huxerui::File directory);
  huxerui::agentkit::Async<std::shared_ptr<const huxerui::agentkit::AgentChatAttachment>> Stage(
      huxerui::FileReference source, huxerui::agentkit::ModelCapabilities capabilities);
  huxerui::agentkit::Async<huxerui::agentkit::ArtifactDraft> Resolve(huxerui::agentkit::ArtifactId id);

  const huxerui::File& Directory() const { return directory_; }

private:
  struct Selection;
  huxerui::File directory_;
  std::map<huxerui::agentkit::ArtifactId, std::weak_ptr<Selection>> drafts_;
};

huxerui::agentkit::ArtifactStore WithAttachmentImports(huxerui::agentkit::ArtifactStore artifacts,
    std::shared_ptr<AttachmentImports> imports);
huxerui::View AttachmentPicker(std::shared_ptr<AttachmentImports> imports,
    huxerui::State<huxerui::agentkit::AgentChatDraftSources> sources, huxerui::agentkit::ModelCapabilities capabilities,
    std::function<void()> on_staged = {});

} // namespace agentkit_preview
