#include "git_process.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using namespace std::chrono_literals;

kit::AgentResult<int> Failed(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<int>::Failure({.code = code, .message = std::move(message)});
}

#if defined(_WIN32)
struct Handle {
  HANDLE value;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }

  bool Valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::wstring QuoteWindowsArgument(const std::wstring& arg) {
  std::wstring quoted = L"\"";
  std::size_t slashes = 0;
  for (auto ch : arg) {
    if (ch == L'\\') { ++slashes; continue; }
    quoted.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
    quoted.push_back(ch);
    slashes = 0;
  }
  quoted.append(2 * slashes, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::vector<wchar_t> GitEnvironment() {
  auto raw = GetEnvironmentStringsW();
  std::vector<std::wstring> entries;
  if (raw) {
    for (const wchar_t* value = raw; *value;) {
      std::wstring entry(value);
      value += entry.size() + 1;
      if (_wcsnicmp(entry.c_str(), L"GIT_", 4) == 0 ||
          _wcsnicmp(entry.c_str(), L"HUXERUI_AGENTKIT_API_KEY=", 25) == 0) continue;
      entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(raw);
  }
  entries.emplace_back(L"GIT_TERMINAL_PROMPT=0");
  entries.emplace_back(L"GIT_NO_LAZY_FETCH=1");
  std::ranges::sort(entries, [](const auto& left, const auto& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  std::vector<wchar_t> result;
  for (const auto& entry : entries) {
    result.insert(result.end(), entry.begin(), entry.end());
    result.push_back(L'\0');
  }
  result.push_back(L'\0');
  return result;
}

kit::AgentResult<int> LaunchGit(const std::vector<std::string>& arguments, const std::filesystem::path& capture) {
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  Handle output{CreateFileW(capture.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL, nullptr)};
  Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!output.Valid() || !input.Valid()) return Failed(kit::ErrorCode::PersistenceFailure, "Could not open Git process streams");
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<std::byte> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
    return Failed(kit::ErrorCode::ProviderFailure, "Could not prepare the Git process");
  struct Attributes {
    LPPROC_THREAD_ATTRIBUTE_LIST value;
    ~Attributes() { DeleteProcThreadAttributeList(value); }
  } cleanup{attributes};
  HANDLE streams[]{input.value, output.value};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, streams, sizeof(streams), nullptr, nullptr))
    return Failed(kit::ErrorCode::ProviderFailure, "Could not attach Git process streams");

  std::wstring line = L"git.exe";
  for (const auto& argument : arguments) {
    line += L" " + QuoteWindowsArgument(std::filesystem::u8path(argument).wstring());
  }
  auto environment = GitEnvironment();
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.value;
  startup.StartupInfo.hStdError = output.value;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION launched{};
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr,
      &startup.StartupInfo, &launched))
    return Failed(kit::ErrorCode::Unsupported, "Could not start Git. Check that Git is available to Preview");
  Handle process{launched.hProcess};
  Handle thread{launched.hThread};
  if (WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0) {
    if (TerminateProcess(process.value, 1)) WaitForSingleObject(process.value, 2000);
    return Failed(kit::ErrorCode::ResultUnknown, "Git did not finish in time. Check the worktree before retrying");
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(process.value, &code))
    return Failed(kit::ErrorCode::ResultUnknown, "Could not confirm the Git exit status");
  return kit::AgentResult<int>::Success(static_cast<int>(code));
}
#elif (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)
kit::AgentResult<int> LaunchGit(const std::vector<std::string>& arguments, const std::filesystem::path& capture) {
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0)
    return Failed(kit::ErrorCode::ProviderFailure, "Could not prepare the Git process");
  struct Actions {
    posix_spawn_file_actions_t& value;
    ~Actions() { posix_spawn_file_actions_destroy(&value); }
  } cleanup{actions};
  if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, capture.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600) != 0 ||
      posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) != 0)
    return Failed(kit::ErrorCode::PersistenceFailure, "Could not attach Git process streams");

  std::vector<char*> argv{const_cast<char*>("git")};
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  std::vector<char*> env;
  for (auto value = environ; *value; ++value) {
    const std::string_view entry(*value);
    if (!entry.starts_with("GIT_") && !entry.starts_with("HUXERUI_AGENTKIT_API_KEY=")) env.push_back(*value);
  }
  char terminal_prompt[] = "GIT_TERMINAL_PROMPT=0";
  char lazy_fetch[] = "GIT_NO_LAZY_FETCH=1";
  env.insert(env.end(), {terminal_prompt, lazy_fetch, nullptr});

  pid_t child;
  if (posix_spawnp(&child, "git", &actions, nullptr, argv.data(), env.data()) != 0)
    return Failed(kit::ErrorCode::Unsupported, "Could not start Git. Check that Git is available to Preview");
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (WIFEXITED(status)) return kit::AgentResult<int>::Success(WEXITSTATUS(status));
      return Failed(kit::ErrorCode::ResultUnknown, "Git ended without a normal exit");
    }
    if (waited < 0 && errno != EINTR) return Failed(kit::ErrorCode::ResultUnknown, "Could not confirm the Git exit status");
    std::this_thread::sleep_for(15ms);
  }
  kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return Failed(kit::ErrorCode::ResultUnknown, "Git did not finish in time. Check the worktree before retrying");
}
#else
kit::AgentResult<int> LaunchGit(const std::vector<std::string>&, const std::filesystem::path&) {
  return Failed(kit::ErrorCode::Unsupported, "Worktree allocation requires desktop Git");
}
#endif

std::string Utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

} // namespace

kit::AgentResult<GitResult> RunGit(huxerui::File scratch, std::filesystem::path directory,
    std::vector<std::string> arguments) {
  using Result = kit::AgentResult<GitResult>;
  const auto output = scratch.Child(kit::NewCommandId().value + ".git-output");
  struct Capture {
    huxerui::File file;
    ~Capture() { static_cast<void>(file.Delete()); }
  } capture{output};
  arguments.insert(arguments.begin(), {"-C", Utf8(directory), "-c", "core.fsmonitor=false",
      "-c", "core.hooksPath=" + scratch.Child("disabled-hooks").Path()});
  auto result = LaunchGit(arguments, std::filesystem::u8path(output.Path()));
  if (!result.Succeeded()) return Result::Failure(result.Error());
  const auto info = output.Stat();
  if (!info.Succeeded())
    return Result::Failure({.code = kit::ErrorCode::PersistenceFailure, .message = "Git diagnostics are unavailable"});
  if (info.Value().size > 1024 * 1024)
    return Result::Failure({.code = kit::ErrorCode::BudgetExceeded, .message = "Git diagnostics exceed the capture limit"});
  auto content = output.ReadString();
  if (!content.Succeeded()) return Result::Failure({.code = kit::ErrorCode::ProviderFailure, .message = "Could not read Git diagnostics"});
  return Result::Success({result.Value(), std::move(content).Value()});
}

} // namespace agentkit_preview
