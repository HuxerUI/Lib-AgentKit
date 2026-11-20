#include "preview_store_codec.h"
#include "browser_workspace.h"

#include <emscripten.h>

#include <charconv>
#include <unordered_map>

namespace agentkit_preview {
namespace {
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;
using Reply = kit::AgentResult<Json>;
using storage::Fail;

std::unordered_map<std::uint32_t, huxerui::CallbackCompletion<Reply>> pending;
std::uint32_t next_request = 0;

extern "C" EMSCRIPTEN_KEEPALIVE void AgentKitPreviewStorageCompleted(std::uint32_t id, int success, const char* text) {
  auto request = pending.extract(id);
  if (request.empty()) return;
  if (!success) {
    request.mapped().Complete(Fail<Json>(kit::ErrorCode::PersistenceFailure, text));
    return;
  }
  auto result = Json::parse(text, nullptr, false);
  if (result.is_discarded()) request.mapped().Complete(Fail<Json>(kit::ErrorCode::PersistenceFailure, "Invalid browser storage response"));
  else request.mapped().Complete(Reply::Success(std::move(result)));
}

EM_JS_DEPS(agentkit_storage_bridge, "$UTF8ToString,$stringToNewUTF8,free");
EM_JS(void, StartStorageOperation, (std::uint32_t id, const char* database, const char* operation, const char* request), {
  const complete = (success, text) => {
    const pointer = stringToNewUTF8(text);
    try {
      _AgentKitPreviewStorageCompleted(id, success, pointer);
    } finally {
      _free(pointer);
    }
  };
  try {
    const storage = Module['agentkitStorage'];
    if (!storage) throw new Error('Browser storage bridge is unavailable');
    storage(UTF8ToString(database), UTF8ToString(operation), JSON.parse(UTF8ToString(request)))
      .then((value) => complete(1, JSON.stringify(value)), (error) => complete(0, error?.message ?? String(error)));
  } catch (error) {
    complete(0, error?.message ?? String(error));
  }
});

kit::Async<Json> Request(const std::string& database, const char* operation, Json request = nullptr) {
  auto start = [database, operation, request = request.dump()](huxerui::CallbackCompletion<Reply> reply) {
    do { ++next_request; } while (pending.contains(next_request));
    const auto id = next_request;
    pending.emplace(id, std::move(reply));
    StartStorageOperation(id, database.c_str(), operation, request.c_str());
    return std::function<void()>{[id] { pending.erase(id); }};
  };
  co_return co_await huxerui::AwaitCallback<Reply>(std::move(start));
}

kit::Revision DecodeRevision(const Json& value) {
  const auto& encoded = value.get_ref<const std::string&>();
  kit::Revision revision = 0;
  const auto end = encoded.data() + encoded.size();
  const auto parsed = std::from_chars(encoded.data(), end, revision);
  if (parsed.ec != std::errc{} || parsed.ptr != end || revision > storage::max_revision)
    throw std::invalid_argument("Invalid browser store revision");
  return revision;
}

std::string EncodeBytes(const std::vector<std::byte>& bytes) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (std::byte byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    encoded.push_back(hex[value >> 4]);
    encoded.push_back(hex[value & 0x0f]);
  }
  return encoded;
}

std::vector<std::byte> DecodeBytes(const std::string& encoded) {
  if (encoded.size() % 2) throw std::invalid_argument("Invalid browser store data");
  std::vector<std::byte> bytes;
  bytes.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    unsigned byte = 0;
    const auto start = encoded.data() + offset;
    const auto result = std::from_chars(start, start + 2, byte, 16);
    if (result.ec != std::errc{} || result.ptr != start + 2) throw std::invalid_argument("Invalid browser store data");
    bytes.push_back(static_cast<std::byte>(byte));
  }
  return bytes;
}

class BrowserSessionStore final : public kit::SessionStore {
public:
  explicit BrowserSessionStore(std::string database) : database_(std::move(database)) {}

  kit::Async<kit::StoreImage> LoadAsync() override {
    auto reply = co_await Request(database_, "load");
    if (!reply.Succeeded()) co_return kit::AgentResult<kit::StoreImage>::Failure(reply.Error());
    try {
      const auto& image = reply.Value();
      kit::StoreImage loaded{DecodeRevision(image.at("revision")), {}};
      for (const auto& record : image.at("records").get_ref<const Json::array_t&>()) {
        loaded.records.push_back({record.at("collection").get<std::string>(), record.at("key").get<std::string>(),
            DecodeRevision(record.at("revision")), DecodeBytes(record.at("data").get_ref<const std::string&>())});
      }
      co_return kit::AgentResult<kit::StoreImage>::Success(std::move(loaded));
    } catch (const std::exception& error) {
      co_return Fail<kit::StoreImage>(kit::ErrorCode::PersistenceFailure, error.what());
    }
  }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction transaction) override {
    auto valid = storage::NormalizeCommit(transaction);
    if (!valid.Succeeded()) co_return kit::AgentResult<kit::Revision>::Failure(valid.Error());
    auto writes = Json::array();
    for (auto& write : transaction.writes) {
      writes.push_back({{"collection", std::move(write.collection)}, {"key", std::move(write.key)},
          {"expected", write.expected ? Json(std::to_string(*write.expected)) : Json(nullptr)},
          {"data", write.data ? Json(EncodeBytes(*write.data)) : Json(nullptr)}});
    }
    Json payload{{"id", std::move(transaction.commit.value)}, {"expected", std::to_string(transaction.expected_store_revision)},
                 {"writes", std::move(writes)}};
    auto reply = co_await Request(database_, "commit", std::move(payload));
    if (!reply.Succeeded()) co_return kit::AgentResult<kit::Revision>::Failure(reply.Error());
    try {
      const auto& result = reply.Value();
      const auto& revision = result.at("revision");
      if (auto conflict = result.find("conflict"); conflict != result.end()) {
        co_return Fail<kit::Revision>(kit::ErrorCode::Conflict, conflict->get<std::string>(),
            revision.is_null() ? std::nullopt : std::optional{DecodeRevision(revision)});
      }
      co_return kit::AgentResult<kit::Revision>::Success(DecodeRevision(revision));
    } catch (const std::exception& error) {
      co_return Fail<kit::Revision>(kit::ErrorCode::CommitUncertain, error.what());
    }
  }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId commit) override {
    using Result = kit::AgentResult<std::optional<kit::Revision>>;
    if (commit.value.empty()) co_return Fail<std::optional<kit::Revision>>(kit::ErrorCode::InvalidArgument, "Commit ID is required");
    Json payload{{"id", std::move(commit.value)}};
    auto reply = co_await Request(database_, "findCommit", std::move(payload));
    if (!reply.Succeeded()) co_return Result::Failure(reply.Error());
    try {
      co_return Result::Success(reply.Value().is_null() ? std::nullopt : std::optional{DecodeRevision(reply.Value())});
    } catch (const std::exception& error) {
      co_return Fail<std::optional<kit::Revision>>(kit::ErrorCode::PersistenceFailure, error.what());
    }
  }

private:
  std::string database_;
};

class BrowserComposerPreferences final : public ComposerPreferences {
public:
  explicit BrowserComposerPreferences(std::string database) : database_(std::move(database)) {}

  kit::Async<Entries> LoadAsync(kit::PrincipalId principal) override {
    Json request{{"principal", principal.value}};
    auto reply = co_await Request(database_, "loadPreferences", std::move(request));
    if (!reply.Succeeded()) co_return kit::AgentResult<Entries>::Failure(reply.Error());
    try {
      Entries entries;
      for (const auto& value : reply.Value().get_ref<const Json::array_t&>()) {
        kit::SessionId session{value.at("session").get<std::string>()};
        kit::UserRunOptions options;
        const auto& model = value.at("model");
        const auto& profile = value.at("profile");
        if (!model.is_null()) options.model = kit::ModelProviderRef{{model.at("id").get<std::string>()}, DecodeRevision(model.at("version"))};
        if (!profile.is_null()) options.execution_profile = kit::ExecutionProfileRef{{profile.at("id").get<std::string>()}, DecodeRevision(profile.at("version"))};
        if (value.at("principal") != principal.value || !storage::ValidPreference(principal, session, options))
          throw std::invalid_argument("Invalid saved run configuration");
        entries.emplace(std::move(session), std::move(options));
      }
      co_return kit::AgentResult<Entries>::Success(std::move(entries));
    } catch (const std::exception& error) {
      co_return Fail<Entries>(kit::ErrorCode::PersistenceFailure, error.what());
    }
  }

  kit::Async<void> SaveAsync(kit::PrincipalId principal, kit::SessionId session, kit::UserRunOptions options) override {
    if (!storage::ValidPreference(principal, session, options))
      co_return Fail<void>(kit::ErrorCode::InvalidArgument, "A valid run configuration is required");
    const auto encode = [](const auto& ref) {
      return ref ? Json{{"id", ref->id.value}, {"version", std::to_string(ref->version)}} : Json(nullptr);
    };
    Json request{{"principal", std::move(principal.value)}, {"session", std::move(session.value)},
                 {"model", encode(options.model)}, {"profile", encode(options.execution_profile)}};
    auto reply = co_await Request(database_, "savePreference", std::move(request));
    if (!reply.Succeeded()) co_return kit::AgentResult<void>::Failure(reply.Error());
    co_return kit::AgentResult<void>::Success();
  }

private:
  std::string database_;
};

kit::AgentResult<NavigationPreferences> ReadNavigation(const Json& value) {
  if (value.is_null()) return kit::AgentResult<NavigationPreferences>::Success({});
  if (!value.is_string()) return Fail<NavigationPreferences>(kit::ErrorCode::PersistenceFailure, "Invalid conversation navigation");
  return storage::DecodeNavigation(Json::parse(value.get_ref<const std::string&>(), nullptr, false));
}

class BrowserNavigationStore final : public ConversationNavigationStore {
public:
  explicit BrowserNavigationStore(std::string database) : database_(std::move(database)) {}

  kit::Async<NavigationPreferences> LoadAsync(kit::PrincipalId principal) override {
    Json key{{"principal", std::move(principal.value)}};
    auto reply = co_await Request(database_, "loadNavigation", std::move(key));
    if (!reply.Succeeded()) co_return kit::AgentResult<NavigationPreferences>::Failure(reply.Error());
    co_return ReadNavigation(reply.Value());
  }

  kit::Async<NavigationPreferences> EditAsync(kit::PrincipalId principal, NavigationEdit edit) override {
    using Result = kit::AgentResult<NavigationPreferences>;
    Json key{{"principal", principal.value}};
    for (int attempt = 0; attempt < 8; ++attempt) {
      auto prior = co_await Request(database_, "loadNavigation", key);
      if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
      auto navigation = ReadNavigation(prior.Value());
      if (!navigation.Succeeded()) co_return navigation;
      auto changed = edit(navigation.Value());
      if (!changed.Succeeded()) co_return Result::Failure(changed.Error());
      Json request{{"principal", principal.value}, {"expected", std::move(prior).Value()},
                   {"value", storage::EncodeNavigation(navigation.Value()).dump()}};
      auto saved = co_await Request(database_, "saveNavigation", std::move(request));
      if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
      if (saved.Value() == true) co_return navigation;
    }
    co_return Fail<NavigationPreferences>(kit::ErrorCode::Conflict, "Conversation navigation was edited in another tab");
  }

private:
  std::string database_;
};

template<class T, class Decode>
kit::Async<T> WorkspaceRequest(huxerui::File store, const char* operation, Json payload, Decode decode) {
  Reply reply = co_await Request("agentkit-preview:" + store.Path(), operation, std::move(payload));
  if (!reply.Succeeded()) co_return kit::AgentResult<T>::Failure(reply.Error());
  try {
    if (const Json& value = reply.Value(); value.is_object() && value.contains("error")) {
      const auto error = value.at("error").get<std::string>();
      auto code = kit::ErrorCode::ProviderFailure;
      if (error == "permission") code = kit::ErrorCode::PermissionDenied;
      else if (error == "notFound") code = kit::ErrorCode::NotFound;
      else if (error == "unsupported") code = kit::ErrorCode::Unsupported;
      else if (error == "invalid") code = kit::ErrorCode::InvalidArgument;
      else if (error == "budget") code = kit::ErrorCode::BudgetExceeded;
      co_return Fail<T>(code, value.at("message").get<std::string>());
    }
    co_return decode(reply.Value());
  } catch (const std::exception& error) {
    co_return Fail<T>(kit::ErrorCode::PersistenceFailure, error.what());
  }
}

BrowserWorkspace DecodeWorkspace(const Json& value) {
  return {{value.at("id").get<std::string>()}, value.at("name").get<std::string>()};
}
} // namespace

kit::Async<PreviewStores> OpenPreviewStoresAsync(huxerui::File file) {
  auto database = "agentkit-preview:" + file.Path();
  auto opened = co_await Request(database, "open");
  if (!opened.Succeeded()) co_return kit::AgentResult<PreviewStores>::Failure(opened.Error());
  co_return kit::AgentResult<PreviewStores>::Success({std::make_shared<BrowserSessionStore>(database),
      std::make_shared<BrowserComposerPreferences>(database), std::make_shared<BrowserNavigationStore>(std::move(database))});
}

kit::Async<std::vector<BrowserWorkspace>> LoadBrowserWorkspaces(huxerui::File store) {
  co_return co_await WorkspaceRequest<std::vector<BrowserWorkspace>>(std::move(store), "loadWorkspaces", nullptr,
      [](const Json& value) {
        std::vector<BrowserWorkspace> folders;
        for (const auto& entry : value.get_ref<const Json::array_t&>()) folders.push_back(DecodeWorkspace(entry));
        return kit::AgentResult<std::vector<BrowserWorkspace>>::Success(std::move(folders));
      });
}

kit::Async<std::optional<BrowserWorkspace>> PickBrowserWorkspace(huxerui::File store) {
  Json request{{"id", kit::NewCommandId().value}};
  co_return co_await WorkspaceRequest<std::optional<BrowserWorkspace>>(std::move(store), "pickWorkspace", std::move(request),
      [](const Json& value) {
        return kit::AgentResult<std::optional<BrowserWorkspace>>::Success(value.is_null()
            ? std::nullopt : std::optional{DecodeWorkspace(value)});
      });
}

kit::Async<void> AuthorizeBrowserWorkspace(huxerui::File store, kit::WorkspaceId workspace) {
  Json request{{"id", std::move(workspace.value)}};
  co_return co_await WorkspaceRequest<void>(std::move(store), "authorizeWorkspace", std::move(request),
      [](const Json&) { return kit::AgentResult<void>::Success(); });
}

kit::Async<std::vector<BrowserWorkspaceEntry>> ListBrowserWorkspaceReports(
    huxerui::File store, kit::WorkspaceId workspace, std::string path) {
  Json request{{"id", std::move(workspace.value)}, {"path", std::move(path)}};
  co_return co_await WorkspaceRequest<std::vector<BrowserWorkspaceEntry>>(std::move(store), "listWorkspaceReports", std::move(request),
      [](const Json& value) {
        std::vector<BrowserWorkspaceEntry> entries;
        for (const auto& entry : value.get_ref<const Json::array_t&>())
          entries.push_back({entry.at("name").get<std::string>(), entry.at("directory").get<bool>()});
        return kit::AgentResult<std::vector<BrowserWorkspaceEntry>>::Success(std::move(entries));
      });
}

kit::Async<BrowserWorkspaceReport> ReadBrowserWorkspaceReport(
    huxerui::File store, kit::WorkspaceId workspace, std::string path) {
  Json request{{"id", std::move(workspace.value)}, {"path", std::move(path)}};
  co_return co_await WorkspaceRequest<BrowserWorkspaceReport>(std::move(store), "readWorkspaceReport", std::move(request),
      [](const Json& value) {
        return kit::AgentResult<BrowserWorkspaceReport>::Success(
            {value.at("name").get<std::string>(), value.at("text").get<std::string>()});
      });
}
} // namespace agentkit_preview
