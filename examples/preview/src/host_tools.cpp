#include "host_tools.h"

#include <app_resources.h>

namespace agentkit_preview {

std::optional<huxerui::StringResource> HostToolLabel(const huxerui::agentkit::ToolId& id) {
  if (id == RunPowerShell) return app::strings::ui_tool_powershell;
  if (id == WebSearch) return app::strings::ui_tool_web_search;
  if (id == WebFetch) return app::strings::ui_tool_web_fetch;
  return {};
}

} // namespace agentkit_preview

#ifdef _WIN32
#include "preview_json.h"

#include <huxerui/clipboard.h>
#include <huxerui/huxerui.h>
#include <huxerui/task.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

namespace agentkit_preview {
namespace {

namespace fs = std::filesystem;
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;
using namespace std::chrono_literals;
using Execution = kit::AgentResult<kit::ToolExecutionResult>;

template <class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

struct Handle {
  HANDLE value = nullptr;

  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};

bool Interrupted(const LocalFileAccess& access, std::string_view revision, std::stop_token stop) {
  return stop.stop_requested() || access.Current()->revision != revision;
}

std::wstring Wide(std::string_view text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(length, L'\0');
  if (length) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::string Utf8(std::wstring_view text) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(length, '\0');
  if (length) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
  return result;
}

std::string CleanText(std::string text) {
  std::erase_if(text, [](unsigned char ch) { return ch < 32 && ch != '\t' && ch != '\r' && ch != '\n'; });
  if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
  return Utf8(Wide(text));
}

Execution Cancelled() {
  return Execution::Success({.outcome = kit::ToolOutcome::Cancelled});
}

Execution ProcessError(std::string message, bool started) {
  return Execution::Success({.outcome = started ? kit::ToolOutcome::ResultUnknown : kit::ToolOutcome::Failed,
      .reason = kit::AgentError{.code = started ? kit::ErrorCode::ResultUnknown : kit::ErrorCode::ProviderFailure,
          .message = std::move(message)}});
}

std::optional<fs::path> PowerShellPath() {
  std::array<wchar_t, MAX_PATH> folder{};
  const auto length = GetSystemDirectoryW(folder.data(), static_cast<UINT>(folder.size()));
  if (!length || length >= folder.size()) return {};
  return fs::path(std::wstring_view{folder.data(), length}) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
}

std::wstring PowerShellCommand(const fs::path& executable, std::string_view command) {
  std::wstring script = L"[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)\n" + Wide(command);
  DWORD size = 0;
  const auto* bytes = reinterpret_cast<const BYTE*>(script.data());
  const auto length = static_cast<DWORD>(script.size() * sizeof(wchar_t));
  constexpr auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
  if (!CryptBinaryToStringW(bytes, length, flags, nullptr, &size)) return {};
  std::wstring encoded(size, L'\0');
  if (!CryptBinaryToStringW(bytes, length, flags, encoded.data(), &size)) return {};
  encoded.resize(size);
  return L"\"" + executable.wstring() + L"\" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand " + encoded;
}

std::vector<wchar_t> PowerShellEnvironment() {
  constexpr std::wstring_view names[]{L"APPDATA", L"CommonProgramFiles", L"COMSPEC", L"HOMEDRIVE", L"HOMEPATH", L"LOCALAPPDATA",
      L"NUMBER_OF_PROCESSORS", L"OS", L"PATH", L"PATHEXT", L"PROCESSOR_ARCHITECTURE", L"ProgramData", L"ProgramFiles",
      L"ProgramFiles(x86)", L"SystemDrive", L"SystemRoot", L"TEMP", L"TMP", L"USERNAME", L"USERPROFILE", L"WINDIR"};
  std::vector<wchar_t> result;
  for (auto name : names) {
    auto size = GetEnvironmentVariableW(name.data(), nullptr, 0);
    if (!size) continue;
    std::wstring value(size, L'\0');
    const auto count = GetEnvironmentVariableW(name.data(), value.data(), size);
    if (count >= size) continue;
    result.insert(result.end(), name.begin(), name.end());
    result.push_back(L'=');
    result.insert(result.end(), value.begin(), value.begin() + count);
    result.push_back(L'\0');
  }
  if (result.empty()) result.push_back(L'\0');
  result.push_back(L'\0');
  return result;
}

struct ProcessPipe {
  Handle read, write;
  std::string text;
  bool truncated = false;

  bool Open(SECURITY_ATTRIBUTES& inherit) {
    return CreatePipe(&read.value, &write.value, &inherit, 0) &&
        SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0);
  }

  bool Drain() {
    std::array<char, 8192> buffer{};
    DWORD available = 0;
    if (!PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr))
      return GetLastError() == ERROR_BROKEN_PIPE;
    if (!available) return true;
    DWORD count = 0;
    if (!ReadFile(read.value, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &count, nullptr))
      return GetLastError() == ERROR_BROKEN_PIPE;
    constexpr std::size_t max_output = 16 << 10;
    const auto keep = std::min<std::size_t>(count, max_output - text.size());
    text.append(buffer.data(), keep);
    truncated |= keep < count;
    return true;
  }
};

Execution ExecutePowerShell(Json plan, std::stop_token stop, std::shared_ptr<LocalFileAccess> access) {
  const auto revision = plan.at("revision").get<std::string>();
  const auto stopped = [&] { return Interrupted(*access, revision, stop); };
  if (stopped()) return Cancelled();
  const auto folder = plan.at("directory").get<std::string>();
  const huxerui::File directory(folder);
  if (const auto task = plan.find("task"); task != plan.end()) {
    auto task_folder = access->ResolveTaskDirectory({task->get<std::string>()});
    if (!task_folder.Succeeded() || task_folder.Value() != folder || !directory.CreateDirectories())
      return ProcessError("Could not create the task folder for PowerShell", false);
  }
  const auto resolved = LocalDirectory(directory);
  if (!resolved.Succeeded() || resolved.Value() != folder)
    return ProcessError("The approved PowerShell working directory is unavailable or changed", false);

  const auto executable = PowerShellPath();
  if (!executable) return ProcessError("Windows PowerShell is unavailable", false);
  auto line = PowerShellCommand(*executable, plan.at("command").get<std::string>());
  if (line.empty()) return ProcessError("Could not encode the PowerShell command", false);

  Handle job{CreateJobObjectW(nullptr, nullptr)};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    return ProcessError("Could not supervise the PowerShell process", false);
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  ProcessPipe output, errors;
  if (input.value == INVALID_HANDLE_VALUE || !output.Open(inherit) || !errors.Open(inherit))
    return ProcessError("Could not open PowerShell streams", false);
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<std::byte> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
    return ProcessError("Could not prepare PowerShell streams", false);
  struct Attributes {
    LPPROC_THREAD_ATTRIBUTE_LIST value;
    ~Attributes() { DeleteProcThreadAttributeList(value); }
  } cleanup{attributes};
  HANDLE streams[]{input.value, output.write.value, errors.write.value};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, streams, sizeof(streams), nullptr, nullptr))
    return ProcessError("Could not attach PowerShell streams", false);

  auto environment = PowerShellEnvironment();
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.write.value;
  startup.StartupInfo.hStdError = errors.write.value;
  startup.lpAttributeList = attributes;
  if (stopped()) return Cancelled();
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable->c_str(), line.data(), nullptr, nullptr, TRUE,
          EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
          environment.data(), Wide(folder).c_str(), &startup.StartupInfo, &process))
    return ProcessError("Could not start Windows PowerShell", false);
  Handle child{process.hProcess};
  Handle thread{process.hThread};
  if (!AssignProcessToJobObject(job.value, child.value)) {
    TerminateProcess(child.value, 1);
    return ProcessError("Could not attach PowerShell to its process job", false);
  }
  if (stopped()) return Cancelled();
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
    return ProcessError("Could not resume PowerShell", false);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(plan.at("timeout_seconds").get<int>());
  for (;;) {
    if (stopped() || std::chrono::steady_clock::now() >= deadline)
      return ProcessError("PowerShell stopped before completion. Check its effects before retrying.", true);
    if (!output.Drain() || !errors.Drain()) return ProcessError("Could not read the PowerShell process streams", true);
    const auto wait = WaitForSingleObject(child.value, 15);
    if (wait == WAIT_OBJECT_0) break;
    if (wait != WAIT_TIMEOUT) return ProcessError("Could not confirm PowerShell completion", true);
  }
  TerminateJobObject(job.value, 1);
  for (int chunk = 0; chunk < 8; ++chunk) {
    if (!output.Drain() || !errors.Drain()) return ProcessError("Could not read final PowerShell output", true);
  }
  if (stopped()) return ProcessError("PowerShell authorization changed. Check its effects before retrying.", true);
  DWORD code = 0;
  if (!GetExitCodeProcess(child.value, &code)) return ProcessError("Could not confirm the PowerShell exit code", true);
  auto stdout_text = CleanText(std::move(output.text));
  auto stderr_text = CleanText(std::move(errors.text));
  const auto summary = "PowerShell exited with code " + std::to_string(code);
  // The command finished and its exit status is known. Keep its diagnostics available to the model.
  kit::ToolExecutionResult result{.outcome = kit::ToolOutcome::Succeeded,
      .model_value = DecodeJson(Json{{"exit_code", code}, {"stdout", stdout_text}, {"stderr", stderr_text},
          {"truncated", output.truncated || errors.truncated}}),
      .blocks = {{kit::BlockId{kit::NewCommandId().value}, kit::TextBlock{summary, false}}}};
  result.blocks.push_back({{kit::NewCommandId().value}, kit::TextBlock{folder + "\n" + plan.at("command").get<std::string>(), false}});
  if (!stdout_text.empty()) result.blocks.push_back({{kit::NewCommandId().value}, kit::TextBlock{std::move(stdout_text), false}});
  if (!stderr_text.empty()) result.blocks.push_back({{kit::NewCommandId().value}, kit::TextBlock{std::move(stderr_text), false}});
  return Execution::Success(std::move(result));
}

struct HttpEndpoint {
  std::wstring host;
  std::wstring target;
  INTERNET_PORT port;
  bool secure;
};

std::optional<HttpEndpoint> ParseEndpoint(std::string_view url) {
  if (url.size() > 4096 || std::ranges::any_of(url, [](unsigned char ch) { return ch <= 32 || ch == 127 || ch == '\\'; }))
    return {};
  auto text = Wide(url.substr(0, url.find('#')));
  URL_COMPONENTSW fields{};
  fields.dwStructSize = sizeof(fields);
  fields.dwHostNameLength = fields.dwUrlPathLength = fields.dwExtraInfoLength = static_cast<DWORD>(-1);
  fields.dwUserNameLength = fields.dwPasswordLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &fields) || !fields.dwHostNameLength ||
      fields.dwUserNameLength || fields.dwPasswordLength ||
      (fields.nScheme != INTERNET_SCHEME_HTTP && fields.nScheme != INTERNET_SCHEME_HTTPS))
    return {};
  HttpEndpoint result{
      std::wstring(fields.lpszHostName, fields.dwHostNameLength),
      fields.dwUrlPathLength ? std::wstring(fields.lpszUrlPath, fields.dwUrlPathLength) : L"/",
      fields.nPort, fields.nScheme == INTERNET_SCHEME_HTTPS};
  if (fields.dwExtraInfoLength) result.target.append(fields.lpszExtraInfo, fields.dwExtraInfoLength);
  return result;
}

struct HttpHandle {
  HINTERNET value;
  ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};

struct HttpBody {
  std::string bytes;
  std::string content_type;
  bool truncated = false;
};

kit::AgentResult<HttpBody> HttpGet(const HttpEndpoint& url, const std::wstring& headers,
    const LocalFileAccess& access, const std::string& revision, std::stop_token stop) {
  using Result = kit::AgentResult<HttpBody>;
  const auto deadline = std::chrono::steady_clock::now() + 25s;
  const auto stopped = [&] { return Interrupted(access, revision, stop); };
  const auto failure = [&](std::string message) {
    return Fail<HttpBody>(kit::ErrorCode::ProviderFailure, std::move(message));
  };
  HttpHandle session{WinHttpOpen(L"GanWork/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000))
    return failure("Could not initialize the web request");
  HttpHandle connection{WinHttpConnect(session.value, url.host.c_str(), url.port, 0)};
  if (!connection.value) return failure("Could not open the web connection");
  HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", url.target.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0)};
  DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
  if (!request.value || !WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
    return failure("Could not prepare the web request");
  if (stopped()) return failure("Web request cancelled");
  if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || stopped() || !WinHttpReceiveResponse(request.value, nullptr))
    return failure("The web request failed or timed out");

  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
    return failure("The web server did not provide an HTTP status");
  if (status >= 300 && status < 400) return failure("The server redirected the approved URL. Request the destination separately.");
  if (status < 200 || status >= 300) return failure("Web request returned HTTP " + std::to_string(status));

  HttpBody body;
  std::array<wchar_t, 256> content_type{};
  size = sizeof(content_type);
  if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
          content_type.data(), &size, WINHTTP_NO_HEADER_INDEX))
    body.content_type = Utf8(content_type.data());

  constexpr std::size_t max_body = 1 << 20;
  std::array<char, 8192> buffer{};
  while (!stopped() && std::chrono::steady_clock::now() < deadline) {
    DWORD count = 0;
    if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count))
      return failure("Could not read the web response");
    if (stopped()) break;
    if (!count) return Result::Success(std::move(body));
    const auto keep = std::min<std::size_t>(count, max_body - body.bytes.size());
    body.bytes.append(buffer.data(), keep);
    if (keep < count) {
      body.truncated = true;
      return Result::Success(std::move(body));
    }
  }
  return failure("Web response timed out");
}

std::string QueryParameter(std::string_view text) {
  constexpr std::string_view hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char ch : text) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      result += static_cast<char>(ch);
    } else {
      result += '%';
      result += hex[ch >> 4];
      result += hex[ch & 15];
    }
  }
  return result;
}

std::string LowerAscii(std::string text) {
  for (auto& ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return text;
}

std::string PageText(std::string bytes) {
  auto lower = LowerAscii(bytes);
  for (const std::string_view tag : {"head", "script", "style", "template", "iframe", "noscript", "svg"}) {
    const std::string opener = "<" + std::string(tag);
    const std::string closer = "</" + std::string(tag);
    std::size_t start = 0;
    while ((start = lower.find(opener, start)) != std::string::npos) {
      const auto tag_end = start + opener.size();
      if (tag_end < lower.size() && std::string_view{" \t\r\n>/"}.find(lower[tag_end]) == std::string_view::npos) {
        start = tag_end;
        continue;
      }
      const auto close = lower.find(closer, tag_end);
      const auto end = close == std::string::npos ? close : lower.find('>', close + closer.size());
      const auto length = end == std::string::npos ? bytes.size() - start : end + 1 - start;
      bytes.erase(start, length);
      lower.erase(start, length);
    }
  }
  auto rich = huxerui::ClipboardItem{.html = std::move(bytes)}.ToAttributedText();
  return rich.Succeeded() ? CleanText(rich.Value().PlainText()) : std::string{};
}

bool LimitText(std::string& text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) return false;
  while ((static_cast<unsigned char>(text[max_bytes]) & 0xc0U) == 0x80U) --max_bytes;
  text.resize(max_bytes);
  return true;
}

Execution WebError(const kit::AgentError& error) {
  return Execution::Success({.outcome = kit::ToolOutcome::Failed, .reason = error});
}

Execution ExecuteWeb(Json plan, std::stop_token stop, std::shared_ptr<LocalFileAccess> access, WebSearchConfiguration search) {
  const auto revision = plan.at("revision").get<std::string>();
  if (Interrupted(*access, revision, stop)) return Cancelled();
  auto url = plan.at("url").get<std::string>();
  auto parsed = ParseEndpoint(url);
  if (!parsed) return Fail<kit::ToolExecutionResult>(kit::ErrorCode::InvalidArgument, "Expected a valid HTTP or HTTPS URL");
  const bool searching = plan.contains("query");
  std::wstring headers = L"Accept: text/html, text/plain, text/markdown, application/json\r\n";
  if (searching) {
    if (!search.Available() || search.endpoint != url)
      return Fail<kit::ToolExecutionResult>(kit::ErrorCode::Conflict, "Web search configuration changed");
    parsed->target += Wide((parsed->target.find(L'?') == std::wstring::npos ? "?" : "&") +
        std::string("q=") + QueryParameter(plan.at("query").get<std::string>()) + "&count=" +
        std::to_string(plan.at("max_results").get<unsigned>()));
    headers = L"Accept: application/json\r\nX-Subscription-Token: " + Wide(search.key) + L"\r\n";
  }
  auto body = HttpGet(*parsed, headers, *access, revision, stop);
  if (Interrupted(*access, revision, stop)) return Cancelled();
  if (!body.Succeeded()) return WebError(body.Error());

  if (searching) {
    const auto response = Json::parse(body.Value().bytes, nullptr, false);
    if (!response.is_object() || !response.contains("web") || !response.at("web").is_object() ||
        !response.at("web").contains("results") || !response.at("web").at("results").is_array())
      return Fail<kit::ToolExecutionResult>(kit::ErrorCode::ProviderFailure, "The search provider did not return a result list");
    auto results = Json::array();
    for (const auto& entry : response.at("web").at("results")) {
      if (!entry.is_object() || !entry.contains("url") || !entry.at("url").is_string()) continue;
      const auto result_url = entry.at("url").get<std::string>();
      if (!ParseEndpoint(result_url)) continue;
      const auto field = [&](const char* key, std::size_t max) {
        auto text = entry.contains(key) && entry.at(key).is_string() ? PageText(entry.at(key).get<std::string>()) : std::string{};
        LimitText(text, max);
        return text;
      };
      results.push_back({{"title", field("title", 400)}, {"url", result_url}, {"snippet", field("description", 800)}});
      if (results.size() >= plan.at("max_results").get<unsigned>()) break;
    }
    return Execution::Success({.outcome = kit::ToolOutcome::Succeeded,
        .model_value = DecodeJson(Json{{"query", plan.at("query")}, {"results", std::move(results)}})});
  }

  auto response = std::move(body).Value();
  auto type = LowerAscii(response.content_type.substr(0, response.content_type.find(';')));
  if (!type.starts_with("text/") && type != "application/json")
    return Fail<kit::ToolExecutionResult>(kit::ErrorCode::Unsupported, "This URL does not return a text page");
  auto page = type == "text/html" ? PageText(std::move(response.bytes)) : CleanText(std::move(response.bytes));
  response.truncated |= LimitText(page, 32000);
  return Execution::Success({.outcome = kit::ToolOutcome::Succeeded,
      .model_value = DecodeJson(Json{{"url", url}, {"text", std::move(page)}, {"truncated", response.truncated}})});
}

using Prepared = kit::AgentResult<kit::PreparedChange>;

std::string Label(huxerui::StringResource label) {
  return huxerui::UseService<huxerui::Resources>()->GetString(label);
}

kit::Async<kit::PreparedChange> PreparePowerShell(const LocalFileAccess& access,
    kit::ToolPrepareContext context, kit::ToolRequest request) {
  auto arguments = EncodeJson(request.arguments);
  const auto command = arguments.at("command").get<std::string>();
  if (command.empty() || command.size() > 6000 || command.find('\0') != std::string::npos)
    co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "Enter a PowerShell command of at most 6000 UTF-8 bytes");
  if (context.readable.size() != 1 || !context.actor.run)
    co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidState, "PowerShell requires a run and a working directory");
  const auto folder = context.readable.front().resource.value;
  const auto timeout = arguments.value("timeout_seconds", 45);
  Json plan{{"command", command}, {"directory", folder}, {"timeout_seconds", timeout}, {"revision", access.Current()->revision}};
  if (!context.readable.front().current) plan["task"] = context.actor.run->value;
  const auto details = huxerui::UseService<huxerui::Resources>()->GetString(app::strings::ui_tool_powershell_approval);
  co_return Prepared::Success({.preview = {.title = Label(app::strings::ui_tool_powershell),
      .summary = details + "\n" + folder + "\n\n" + command},
      .execution_data = DecodeJson(std::move(plan)), .undo_support = kit::UndoSupport::None});
}

kit::Async<kit::PreparedChange> PrepareWeb(const LocalFileAccess& access, const WebSearchConfiguration* search,
    kit::ToolRequest request) {
  const auto args = EncodeJson(request.arguments);
  const auto url = search ? search->endpoint : args.at("url").get<std::string>();
  if (!ParseEndpoint(url) || (search && !search->Available()))
    co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "The web URL or search endpoint is invalid");
  auto summary = url;
  Json plan{{"url", url}, {"revision", access.Current()->revision}};
  if (search) {
    const auto query = args.at("query").get<std::string>();
    if (query.empty() || query.size() > 400 || std::ranges::any_of(query, [](unsigned char ch) { return ch < 32 || ch == 127; }))
      co_return Fail<kit::PreparedChange>(kit::ErrorCode::InvalidArgument, "Enter a search query of at most 400 UTF-8 bytes");
    summary += "\n" + query;
    plan["query"] = query;
    plan["max_results"] = args.value("max_results", 5);
  }
  co_return Prepared::Success({.preview = {
      .title = Label(search ? app::strings::ui_tool_web_search : app::strings::ui_tool_web_fetch), .summary = std::move(summary)},
      .execution_data = DecodeJson(std::move(plan)), .undo_support = kit::UndoSupport::None});
}

kit::JsonSchema Schema(Json properties, const char* required) {
  return {DecodeJson(Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", Json::array({required})},
      {"additionalProperties", false}})};
}

} // namespace

bool WebSearchConfiguration::Available() const {
  if (key.empty() || key.size() > 1024 || std::ranges::any_of(key, [](unsigned char ch) { return ch <= 32 || ch >= 127; }))
    return false;
  auto url = ParseEndpoint(endpoint);
  return url && (url->secure || url->host == L"127.0.0.1");
}

kit::ToolResourceResolver HostToolResources(std::shared_ptr<CodeWorkspaces> workspaces,
    std::shared_ptr<LocalFileAccess> access) {
  return [workspaces = std::move(workspaces), access = std::move(access)](kit::ToolInvocation invocation)
      -> kit::Async<std::vector<kit::ResourceAccess>> {
    using Grants = std::vector<kit::ResourceAccess>;
    using Result = kit::AgentResult<Grants>;
    if (invocation.tool.id != RunPowerShell || invocation.phase != kit::ToolAccessPhase::Prepare)
      co_return Result::Success({});
    const auto arguments = EncodeJson(invocation.arguments);
    std::optional<huxerui::File> folder;
    if (arguments.contains("working_directory")) {
      const auto path = arguments.at("working_directory").get<std::string>();
      if (path.empty() || path.size() > 4096 || !fs::u8path(path).is_absolute())
        co_return Fail<Grants>(kit::ErrorCode::InvalidArgument, "The working directory must be an absolute local path");
      folder.emplace(path);
    } else if (invocation.workspace) {
      const auto workspace = workspaces->Find(*invocation.workspace);
      if (workspace) folder = workspace->NativeFolder();
    }
    const bool standalone = !folder;
    auto path = standalone ? access->ResolveTaskDirectory(invocation.run) : LocalDirectory(*folder);
    if (!path.Succeeded()) co_return Result::Failure(path.Error());
    kit::ResourceId id{std::move(path).Value()};
    co_return Result::Success({{id, standalone ? std::nullopt : std::optional{kit::ResourceRef{id, "directory"}},
        kit::ResourceAccessMode::Read}});
  };
}

kit::AgentResult<void> RegisterHostTools(const std::shared_ptr<kit::AgentRuntime>& runtime,
    std::shared_ptr<LocalFileAccess> access, WebSearchConfiguration search) {
  auto shell = runtime->RegisterTool({.id = RunPowerShell, .version = 1,
      .description = "Run a Windows PowerShell command after explicit approval. Returns exit_code, stdout, stderr, and truncated. "
          "The default working directory is the linked project or this run's private task directory.",
      .input_schema = Schema({{"command", {{"type", "string"}}}, {"working_directory", {{"type", "string"}}},
          {"timeout_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}}}}, "command"),
      .effect = kit::ToolEffect::ExternalSubmit,
      .prepare = [access](kit::ToolPrepareContext context, kit::ToolRequest request) {
        return PreparePowerShell(*access, std::move(context), std::move(request));
      },
      .execute = [access](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        auto data = EncodeJson(call.change.execution_data);
        co_return co_await huxerui::RunWorker(ExecutePowerShell, std::move(data), context.StopToken(), access);
      }});
  if (!shell.Succeeded()) return shell;
  auto fetch = runtime->RegisterTool({.id = WebFetch, .version = 1,
      .description = "Read an approved HTTP or HTTPS page as plain text. Returns the URL, text, and whether the result was truncated.",
      .input_schema = Schema({{"url", {{"type", "string"}}}}, "url"), .effect = kit::ToolEffect::Read,
      .prepare = [access](kit::ToolPrepareContext, kit::ToolRequest request) {
        return PrepareWeb(*access, nullptr, std::move(request));
      },
      .execute = [access](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        auto data = EncodeJson(call.change.execution_data);
        co_return co_await huxerui::RunWorker(ExecuteWeb, std::move(data), context.StopToken(), access, WebSearchConfiguration{});
      }});
  if (!fetch.Succeeded()) return fetch;
  return runtime->RegisterTool({.id = WebSearch, .version = 1,
      .description = "Search the web using the configured provider after approval. Returns titles, URLs and short snippets.",
      .input_schema = Schema({{"query", {{"type", "string"}}},
          {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}}}, "query"), .effect = kit::ToolEffect::Read,
      .prepare = [access, search](kit::ToolPrepareContext, kit::ToolRequest request) {
        return PrepareWeb(*access, &search, std::move(request));
      },
      .execute = [access, search](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        auto data = EncodeJson(call.change.execution_data);
        co_return co_await huxerui::RunWorker(ExecuteWeb, std::move(data), context.StopToken(), access, search);
      }});
}

} // namespace agentkit_preview
#endif
