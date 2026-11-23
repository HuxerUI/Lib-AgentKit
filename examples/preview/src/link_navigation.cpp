#include "link_navigation.h"

#include <huxerui/data.h>

#if defined(_WIN32)
#include <filesystem>
#include <windows.h>
#include <shellapi.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <gio/gio.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace agentkit_preview {
namespace {
namespace kit = huxerui::agentkit;

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)) || defined(__EMSCRIPTEN__)
std::optional<huxerui::Uri> WebLink(std::string_view destination) {
  auto uri = huxerui::Uri::Parse(destination);
  if (!uri) return {};
  const auto authority = uri->Authority();
  if (!authority || authority->empty() || authority->starts_with(':') ||
      authority->find('@') != std::string_view::npos) return {};
  std::string scheme(uri->Scheme());
  for (auto& c : scheme) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if (scheme != "https" && scheme != "http") return {};
  return uri;
}

#if defined(__EMSCRIPTEN__)
EM_JS(void, OpenBrowserLink, (const char* uri), {
  const link = document.createElement('a');
  link.href = UTF8ToString(uri);
  link.target = '_blank';
  link.rel = 'noopener noreferrer';
  document.body.appendChild(link);
  link.click();
  link.remove();
});
#endif

kit::AgentResult<void> OpenWebLink(const kit::AgentChatLink& link) {
  using Result = kit::AgentResult<void>;
  const auto uri = WebLink(link.destination);
  if (!uri) return Result::Failure({kit::ErrorCode::PermissionDenied, "This link is unavailable"});
  const auto& address = uri->ToString();
#if defined(_WIN32)
  const std::wstring wide(address.begin(), address.end());
  const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (opened > 32) return Result::Success();
#elif defined(__linux__) && !defined(__ANDROID__)
  if (g_app_info_launch_default_for_uri(address.c_str(), nullptr, nullptr)) return Result::Success();
#elif defined(__EMSCRIPTEN__)
  OpenBrowserLink(address.c_str());
  return Result::Success();
#endif
  return Result::Failure({kit::ErrorCode::InvalidState, "Could not open the link in your browser"});
}
#endif

} // namespace

std::shared_ptr<kit::AgentChatLinkHandler> MakePreviewLinkHandler() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)) || defined(__EMSCRIPTEN__)
  return std::make_shared<kit::AgentChatLinkHandler>(kit::AgentChatLinkHandler{
      .allows = [](const kit::AgentChatLink& link) { return WebLink(link.destination).has_value(); },
      .open = OpenWebLink});
#else
  return {};
#endif
}

bool CanOpenLocalPages() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__))
  return true;
#else
  return false;
#endif
}

bool OpenLocalPage(const huxerui::File& file) {
#if defined(_WIN32)
  namespace fs = std::filesystem;
  const auto path = fs::u8path(file.Path());
  if (!path.is_absolute() || path.native().find(L'"') != std::wstring::npos) return false;
  constexpr auto app_path = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe";
  const auto argument = L"\"" + path.native() + L"\"";
  for (const auto registry : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    std::wstring executable(32768, L'\0');
    DWORD bytes = static_cast<DWORD>(executable.size() * sizeof(wchar_t));
    if (RegGetValueW(registry, app_path, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, executable.data(), &bytes)
        != ERROR_SUCCESS) continue;
    executable.resize(std::char_traits<wchar_t>::length(executable.c_str()));
    const fs::path chrome(executable);
    if (chrome.is_absolute() && _wcsicmp(chrome.filename().c_str(), L"chrome.exe") == 0 &&
        reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", chrome.c_str(), argument.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
      return true;
  }
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
  return g_app_info_launch_default_for_uri(file.ToUri().ToString().c_str(), nullptr, nullptr);
#else
  static_cast<void>(file);
  return false;
#endif
}

} // namespace agentkit_preview
