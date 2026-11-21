#pragma once

#include <huxerui/agentkit/session.h>
#include <huxerui/agentkit/store.h>
#include <huxerui/file.h>

namespace agentkit_preview {

class ComposerPreferences {
public:
  using Entries = std::map<huxerui::agentkit::SessionId, huxerui::agentkit::UserRunOptions>;

  virtual ~ComposerPreferences() = default;
  virtual huxerui::agentkit::Async<Entries> LoadAsync(huxerui::agentkit::PrincipalId principal) = 0;
  virtual huxerui::agentkit::Async<void> SaveAsync(huxerui::agentkit::PrincipalId principal,
      huxerui::agentkit::SessionId session, huxerui::agentkit::UserRunOptions options) = 0;
};

struct ConversationGroup {
  std::string id;
  std::string name;
  bool collapsed = false;
  std::optional<huxerui::agentkit::WorkspaceId> workspace;
  bool operator==(const ConversationGroup&) const = default;
};

struct NavigationPreferences {
  std::vector<ConversationGroup> groups;
  std::map<huxerui::agentkit::SessionId, std::string> memberships;
  bool operator==(const NavigationPreferences&) const = default;
};

using NavigationEdit = std::function<huxerui::agentkit::AgentResult<void>(NavigationPreferences&)>;

class ConversationNavigationStore {
public:
  virtual ~ConversationNavigationStore() = default;
  virtual huxerui::agentkit::Async<NavigationPreferences> LoadAsync(huxerui::agentkit::PrincipalId principal) = 0;
  virtual huxerui::agentkit::Async<NavigationPreferences> EditAsync(huxerui::agentkit::PrincipalId principal, NavigationEdit edit) = 0;
};

struct PreviewStores {
  std::shared_ptr<huxerui::agentkit::SessionStore> sessions;
  std::shared_ptr<ComposerPreferences> preferences;
  std::shared_ptr<ConversationNavigationStore> navigation;
};

huxerui::agentkit::Async<PreviewStores> OpenPreviewStoresAsync(huxerui::File file);

} // namespace agentkit_preview
