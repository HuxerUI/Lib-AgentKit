#include "single_instance.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;
namespace si = agentkit_preview::single_instance;

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

si::Options MakeTestOptions(const std::string& runtime_dir, const std::string& instance_id,
                            int client_read_timeout_ms = 100) {
  si::Options opts;
  opts.instance_id = instance_id;
  opts.runtime_dir = runtime_dir;
  opts.force_enable = true;
  opts.client_read_timeout_ms = client_read_timeout_ms;
  opts.connect_retry_count = 25;
  opts.connect_retry_delay_ms = 15;
  return opts;
}

class TempDir {
 public:
  TempDir() {
    const unsigned long pid =
#if defined(_WIN32)
        static_cast<unsigned long>(::GetCurrentProcessId());
#else
        static_cast<unsigned long>(::getpid());
#endif
    path_ = fs::path("/tmp") / ("agentkit-si-" + std::to_string(pid));
#if defined(_WIN32)
    path_ = fs::temp_directory_path() / ("agentkit-si-" + std::to_string(pid));
#endif
    std::error_code ec;
    fs::remove_all(path_, ec);
    if (!fs::create_directories(path_, ec)) {
      throw std::runtime_error("failed to create temp dir: " + path_.string());
    }
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string Path() const { return path_.string(); }

 private:
  fs::path path_;
};

std::string ResolveSelfExecutable(const char* argv0) {
#if defined(_WIN32)
  std::wstring buf(32768, L'\0');
  const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (len > 0 && len < buf.size()) {
    const int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(utf8_len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(len), out.data(), utf8_len, nullptr, nullptr);
    return out;
  }
  return argv0 != nullptr ? argv0 : "";
#elif defined(__linux__)
  char buf[4096];
  const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return std::string(buf);
  }
  return argv0 != nullptr ? argv0 : "";
#elif defined(__APPLE__)
  uint32_t size = 0;
  ::_NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) == 0) {
    return std::string(buf.c_str());
  }
  return argv0 != nullptr ? argv0 : "";
#else
  return argv0 != nullptr ? argv0 : "";
#endif
}

#if !defined(_WIN32)

std::string PosixLockPath(const std::string& runtime_dir, const std::string& instance_id) {
  return runtime_dir + "/" + instance_id + "-" + std::to_string(static_cast<unsigned long>(::getuid())) + ".lock";
}

std::string PosixSockPath(const std::string& runtime_dir, const std::string& instance_id) {
  return runtime_dir + "/" + instance_id + "-" + std::to_string(static_cast<unsigned long>(::getuid())) + ".sock";
}

int ConnectRawUnixSocket(const std::string& sock_path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (sock_path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool TriggerExternalActivation(const std::string& runtime_dir, const std::string& instance_id) {
  const int fd = ConnectRawUnixSocket(PosixSockPath(runtime_dir, instance_id));
  if (fd < 0) {
    return false;
  }
  static constexpr char kMsg[] = "ACTIVATE\n";
  const ssize_t sent = ::send(fd, kMsg, sizeof(kMsg) - 1, 0);
  ::close(fd);
  return sent == static_cast<ssize_t>(sizeof(kMsg) - 1);
}

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { Cleanup(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept
      : pid_(other.pid_), stdout_fd_(other.stdout_fd_), output_(std::move(other.output_)) {
    other.pid_ = -1;
    other.stdout_fd_ = -1;
  }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      Cleanup();
      pid_ = other.pid_;
      stdout_fd_ = other.stdout_fd_;
      output_ = std::move(other.output_);
      other.pid_ = -1;
      other.stdout_fd_ = -1;
    }
    return *this;
  }

  static ChildProcess Spawn(const std::string& exe, const std::vector<std::string>& args) {
    int pipe_fds[2]{-1, -1};
    Check(::pipe(pipe_fds) == 0, "pipe() for child stdout failed");

    const pid_t pid = ::fork();
    Check(pid >= 0, "fork() failed");
    if (pid == 0) {
      ::close(pipe_fds[0]);
      ::dup2(pipe_fds[1], STDOUT_FILENO);
      ::close(pipe_fds[1]);

      std::vector<char*> c_args;
      c_args.reserve(args.size() + 2);
      c_args.push_back(const_cast<char*>(exe.c_str()));
      for (const auto& a : args) {
        c_args.push_back(const_cast<char*>(a.c_str()));
      }
      c_args.push_back(nullptr);
      ::execv(exe.c_str(), c_args.data());
      ::_exit(127);
    }

    ::close(pipe_fds[1]);
    ChildProcess proc;
    proc.pid_ = pid;
    proc.stdout_fd_ = pipe_fds[0];
    return proc;
  }

  bool WaitForLine(const std::string& needle, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (output_.find(needle) != std::string::npos) {
        return true;
      }
      const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      pollfd pfd{stdout_fd_, POLLIN, 0};
      const int rc = ::poll(&pfd, 1, static_cast<int>(std::max<long long>(1, rem)));
      if (rc > 0 && (pfd.revents & POLLIN) != 0) {
        char buf[256];
        const ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
        if (n > 0) {
          output_.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
          break;
        }
      } else if (rc > 0 && (pfd.revents & POLLHUP) != 0) {
        char buf[256];
        const ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
        if (n > 0) {
          output_.append(buf, static_cast<size_t>(n));
        }
        break;
      }
    }
    return output_.find(needle) != std::string::npos;
  }

  void KillHard() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
    }
  }

  int WaitExit(std::string* stdout_out = nullptr) {
    if (stdout_fd_ >= 0) {
      char buf[256];
      for (;;) {
        const ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
        if (n > 0) {
          output_.append(buf, static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
          continue;
        } else {
          break;
        }
      }
      ::close(stdout_fd_);
      stdout_fd_ = -1;
    }
    int exit_code = -1;
    if (pid_ > 0) {
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
      if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
      }
    }
    if (stdout_out != nullptr) {
      *stdout_out = output_;
    }
    return exit_code;
  }

 private:
  void Cleanup() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      WaitExit();
    }
    if (stdout_fd_ >= 0) {
      ::close(stdout_fd_);
      stdout_fd_ = -1;
    }
  }

  pid_t pid_ = -1;
  int stdout_fd_ = -1;
  std::string output_;
};

#else

std::wstring ToWide(const std::string& input) {
  const int len = ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), len);
  return out;
}

bool TriggerExternalActivation(const std::string&, const std::string& instance_id) {
  const std::wstring event_name = L"Local\\HuxerUI.GanWork." + ToWide(instance_id) + L".ActivateEvent";
  HANDLE evt = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
  if (evt == nullptr) {
    return false;
  }
  const BOOL ok = ::SetEvent(evt);
  ::CloseHandle(evt);
  return ok != 0;
}

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { Cleanup(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept
      : process_(other.process_), stdout_pipe_(other.stdout_pipe_), output_(std::move(other.output_)) {
    other.process_ = nullptr;
    other.stdout_pipe_ = nullptr;
  }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      Cleanup();
      process_ = other.process_;
      stdout_pipe_ = other.stdout_pipe_;
      output_ = std::move(other.output_);
      other.process_ = nullptr;
      other.stdout_pipe_ = nullptr;
    }
    return *this;
  }

  static ChildProcess Spawn(const std::string& exe, const std::vector<std::string>& args) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    Check(::CreatePipe(&read_pipe, &write_pipe, &sa, 0) != 0, "CreatePipe failed");
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : args) {
      cmd += " \"" + a + "\"";
    }
    std::wstring wcmd = ToWide(cmd);

    STARTUPINFOW si_info{};
    si_info.cb = sizeof(si_info);
    si_info.dwFlags = STARTF_USESTDHANDLES;
    si_info.hStdOutput = write_pipe;
    si_info.hStdError = write_pipe;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si_info, &pi);
    ::CloseHandle(write_pipe);
    if (!ok) {
      ::CloseHandle(read_pipe);
      throw std::runtime_error("CreateProcessW failed");
    }
    ::CloseHandle(pi.hThread);

    ChildProcess proc;
    proc.process_ = pi.hProcess;
    proc.stdout_pipe_ = read_pipe;
    return proc;
  }

  bool WaitForLine(const std::string& needle, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (output_.find(needle) != std::string::npos) {
        return true;
      }
      DWORD avail = 0;
      if (::PeekNamedPipe(stdout_pipe_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        char buf[256];
        DWORD read_bytes = 0;
        if (::ReadFile(stdout_pipe_, buf, sizeof(buf), &read_bytes, nullptr) && read_bytes > 0) {
          output_.append(buf, read_bytes);
          continue;
        }
      }
      ::Sleep(15);
    }
    return output_.find(needle) != std::string::npos;
  }

  void KillHard() {
    if (process_ != nullptr) {
      ::TerminateProcess(process_, 9);
    }
  }

  int WaitExit(std::string* stdout_out = nullptr) {
    if (process_ != nullptr) {
      ::WaitForSingleObject(process_, INFINITE);
    }
    if (stdout_pipe_ != nullptr) {
      DWORD avail = 0;
      while (::PeekNamedPipe(stdout_pipe_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        char buf[256];
        DWORD read_bytes = 0;
        if (!::ReadFile(stdout_pipe_, buf, sizeof(buf), &read_bytes, nullptr) || read_bytes == 0) {
          break;
        }
        output_.append(buf, read_bytes);
      }
      ::CloseHandle(stdout_pipe_);
      stdout_pipe_ = nullptr;
    }
    DWORD exit_code = static_cast<DWORD>(-1);
    if (process_ != nullptr) {
      ::GetExitCodeProcess(process_, &exit_code);
      ::CloseHandle(process_);
      process_ = nullptr;
    }
    if (stdout_out != nullptr) {
      *stdout_out = output_;
    }
    return static_cast<int>(exit_code);
  }

 private:
  void Cleanup() {
    if (process_ != nullptr) {
      ::TerminateProcess(process_, 9);
      WaitExit();
    }
    if (stdout_pipe_ != nullptr) {
      ::CloseHandle(stdout_pipe_);
      stdout_pipe_ = nullptr;
    }
  }

  HANDLE process_ = nullptr;
  HANDLE stdout_pipe_ = nullptr;
  std::string output_;
};

#endif

int RunChildRace(const std::string& runtime_dir, const std::string& instance_id) {
  std::atomic<int> activations{0};
  const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, instance_id));
  if (res.status == si::StartStatus::ActivatedExisting) {
    std::cout << "RACE_ACTIVATED_EXISTING\n" << std::flush;
    return 0;
  }
  if (res.status != si::StartStatus::Acquired) {
    std::cout << "RACE_FAILED " << res.error << '\n' << std::flush;
    return 2;
  }

  si::SetActivationHandler([&activations]() { activations.fetch_add(1); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (std::chrono::steady_clock::now() < deadline && activations.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  si::Stop();
  if (activations.load() == 0) {
    std::cout << "RACE_NO_ACTIVATION\n" << std::flush;
    return 3;
  }
  std::cout << "RACE_ACQUIRED " << activations.load() << '\n' << std::flush;
  return 0;
}

int RunChildSecondary(const std::string& runtime_dir, const std::string& instance_id) {
  const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, instance_id));
  if (res.status == si::StartStatus::ActivatedExisting) {
    return 0;
  }
  if (res.status == si::StartStatus::Acquired) {
    si::Stop();
    return 10;
  }
  if (res.status == si::StartStatus::Failed) {
    std::cout << res.error << '\n' << std::flush;
    return 11;
  }
  return 12;
}

int RunChildCrash(const std::string& runtime_dir, const std::string& instance_id) {
  const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, instance_id));
  if (res.status != si::StartStatus::Acquired) {
    std::cout << "CRASH_CHILD_FAILED " << res.error << '\n' << std::flush;
    return 2;
  }
  std::cout << "READY\n" << std::flush;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void TestDebugReleaseDefault(const std::string& runtime_dir) {
  Check(si::IsEnabled() == !si::IsDebugBuild(), "IsEnabled must invert IsDebugBuild on desktop");
  if (si::IsDebugBuild()) {
    si::Options opts;
    opts.instance_id = "ganwork-debug-default-check";
    opts.runtime_dir = runtime_dir;
    const auto res = si::StartOrActivate(opts);
    Check(res.status == si::StartStatus::Disabled, "Debug build without force_enable must return Disabled");
    Check(res.ShouldContinue(), "Disabled result must allow startup");
    si::Stop();
  }
}

void TestSimultaneousTwoProcesses(const std::string& exe, const std::string& runtime_dir) {
  const std::string id = "race-case";
  auto p1 = ChildProcess::Spawn(exe, {"--child-race", runtime_dir, id});
  auto p2 = ChildProcess::Spawn(exe, {"--child-race", runtime_dir, id});

  std::string out1;
  std::string out2;
  const int rc1 = p1.WaitExit(&out1);
  const int rc2 = p2.WaitExit(&out2);
  Check(rc1 == 0 && rc2 == 0, "Race children failed: rc1=" + std::to_string(rc1) + " out1=" + out1 +
                                    " rc2=" + std::to_string(rc2) + " out2=" + out2);

  const bool p1_acq = out1.find("RACE_ACQUIRED") != std::string::npos;
  const bool p2_acq = out2.find("RACE_ACQUIRED") != std::string::npos;
  const bool p1_act = out1.find("RACE_ACTIVATED_EXISTING") != std::string::npos;
  const bool p2_act = out2.find("RACE_ACTIVATED_EXISTING") != std::string::npos;
  Check((p1_acq && p2_act) || (p2_acq && p1_act),
        "Expected one primary Acquired and one ActivatedExisting; got out1=" + out1 + " out2=" + out2);
}

void TestActivationBeforeHandlerRegistration(const std::string& exe, const std::string& runtime_dir) {
  const std::string id = "pending-handler";
  const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, id));
  Check(res.status == si::StartStatus::Acquired, "Primary should acquire: " + res.error);
  si::Guard guard;

  // Send activation before registering any handler.
  auto sec = ChildProcess::Spawn(exe, {"--child-secondary", runtime_dir, id});
  std::string sec_out;
  const int sec_rc = sec.WaitExit(&sec_out);
  Check(sec_rc == 0, "Secondary did not report ActivatedExisting: rc=" + std::to_string(sec_rc) + " " + sec_out);

  // Give the background listener a brief moment to accept and buffer the pending activation.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::atomic<int> count{0};
  si::SetActivationHandler([&count]() { count.fetch_add(1); });
  Check(count.load() == 1, "Pending activation must be delivered immediately on SetActivationHandler");

  // Re-registering must not replay an already-consumed pending activation.
  si::SetActivationHandler([&count]() { count.fetch_add(10); });
  Check(count.load() == 1, "Pending activation flag should be cleared after delivery");
}

void TestHungClientSocketDoesNotBlock(const std::string& exe, const std::string& runtime_dir) {
#if !defined(_WIN32)
  {
    const std::string id = "hung-timeout";
    const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, id, 60));
    Check(res.status == si::StartStatus::Acquired, "Primary failed: " + res.error);
    si::Guard guard;

    std::atomic<int> activations{0};
    si::SetActivationHandler([&activations]() { activations.fetch_add(1); });

    const int hung_fd = ConnectRawUnixSocket(PosixSockPath(runtime_dir, id));
    Check(hung_fd >= 0, "Failed to connect hung socket");

    // A real secondary instance must still succeed shortly after the 60ms read timeout expires.
    auto sec = ChildProcess::Spawn(exe, {"--child-secondary", runtime_dir, id});
    const int sec_rc = sec.WaitExit();
    ::close(hung_fd);
    Check(sec_rc == 0, "Secondary failed while hung socket was open, rc=" + std::to_string(sec_rc));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline && activations.load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(activations.load() == 1, "Expected activation after hung client timed out");
  }

  {
    const std::string id = "hung-stop";
    const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, id, 5000));
    Check(res.status == si::StartStatus::Acquired, "Primary failed: " + res.error);

    std::atomic<int> activations{0};
    si::SetActivationHandler([&activations]() { activations.fetch_add(1); });

    const int hung_fd = ConnectRawUnixSocket(PosixSockPath(runtime_dir, id));
    Check(hung_fd >= 0, "Failed to connect hung socket for Stop() test");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const auto t0 = std::chrono::steady_clock::now();
    si::Stop();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ::close(hung_fd);

    Check(elapsed_ms < 500, "Stop() blocked on hung client for " + std::to_string(elapsed_ms) + "ms");
    Check(activations.load() == 0, "Hung client without ACTIVATE message must not fire callback");
  }
#else
  (void)exe;
  (void)runtime_dir;
#endif
}

void TestConcurrentActivationAndShutdown(const std::string& runtime_dir) {
  const std::string id = "concurrent-stop";
  for (int round = 0; round < 15; ++round) {
    const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, id, 100));
    Check(res.status == si::StartStatus::Acquired,
          "Round " + std::to_string(round) + " acquire failed: " + res.error);

    si::SetActivationHandler([]() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });

    std::atomic<bool> stop_sending{false};
    std::thread sender([&]() {
      while (!stop_sending.load()) {
        TriggerExternalActivation(runtime_dir, id);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto t0 = std::chrono::steady_clock::now();
    si::Stop();
    const auto stop_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    stop_sending.store(true);
    sender.join();

    Check(stop_ms < 1000, "Stop() took too long in concurrent activation test: " + std::to_string(stop_ms) + "ms");
  }
}

void TestRestartAfterAbnormalExit(const std::string& exe, const std::string& runtime_dir) {
  const std::string id = "crash-restart";
  {
    auto victim = ChildProcess::Spawn(exe, {"--child-crash", runtime_dir, id});
    Check(victim.WaitForLine("READY\n", 3000), "Crash child did not become ready");
    victim.KillHard();
    victim.WaitExit();
  }

  // After ungraceful termination, a new primary must immediately re-acquire lock and clean stale socket.
  const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, id));
  Check(res.status == si::StartStatus::Acquired,
        "Restart after crash failed to acquire lock: " + res.error);
  si::Guard guard;

  std::atomic<int> activations{0};
  si::SetActivationHandler([&activations]() { activations.fetch_add(1); });

  auto sec = ChildProcess::Spawn(exe, {"--child-secondary", runtime_dir, id});
  std::string sec_out;
  const int sec_rc = sec.WaitExit(&sec_out);
  Check(sec_rc == 0, "Secondary after crash recovery failed: " + sec_out);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < deadline && activations.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Check(activations.load() == 1, "Recovered primary did not receive activation");
}

void TestLockAndIpcInitializationFailures(const std::string& runtime_dir) {
#if !defined(_WIN32)
  // 1. Missing parent runtime directory -> lock open() fails -> StartStatus::Failed.
  {
    const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir + "/missing/dir", "fail-dir"));
    Check(res.status == si::StartStatus::Failed && !res.ShouldContinue(),
          "Expected Failed when runtime_dir does not exist");
  }

  // 2. Socket bind() failure must roll back lock acquisition (not leave lock held).
  {
    const std::string id = "fail-bind";
    const std::string sock_blocker = PosixSockPath(runtime_dir, id);
    Check(::mkdir(sock_blocker.c_str(), 0700) == 0, "Failed to create blocking directory at socket path");

    const auto fail_res = si::StartOrActivate(MakeTestOptions(runtime_dir, id));
    Check(fail_res.status == si::StartStatus::Failed && !fail_res.ShouldContinue(),
          "Expected Failed when bind() target is a directory");

    Check(::rmdir(sock_blocker.c_str()) == 0, "Failed to remove blocking directory");
    const auto retry_res = si::StartOrActivate(MakeTestOptions(runtime_dir, id));
    Check(retry_res.status == si::StartStatus::Acquired,
          "Lock was leaked after bind() failure: " + retry_res.error);
    si::Stop();
  }

  // 3. Lock held without any listener socket -> secondary reports Failed (not Acquired / Disabled).
  {
    const std::string id = "fail-no-ipc";
    const std::string lock_path = PosixLockPath(runtime_dir, id);
    const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    Check(fd >= 0, "Failed to open synthetic lock file");
    Check(::flock(fd, LOCK_EX | LOCK_NB) == 0, "Failed to flock synthetic lock file");

    si::Options opts = MakeTestOptions(runtime_dir, id);
    opts.connect_retry_count = 2;
    opts.connect_retry_delay_ms = 10;
    const auto res = si::StartOrActivate(opts);
    ::close(fd);
    Check(res.status == si::StartStatus::Failed && !res.ShouldContinue(),
          "Expected Failed when lock is held without activation socket");
  }
#else
  // 1. Invalid instance_id -> Failed.
  {
    const auto res = si::StartOrActivate(MakeTestOptions(runtime_dir, "bad\\id"));
    Check(res.status == si::StartStatus::Failed && !res.ShouldContinue(),
          "Expected Failed for invalid Windows instance_id");
  }

  // 2. Mutex held without ActivateEvent -> Failed.
  {
    const std::string id = "win-no-event";
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"Local\\HuxerUI.GanWork.win-no-event.SingleInstance");
    Check(mutex != nullptr, "Failed to create synthetic mutex");
    si::Options opts = MakeTestOptions(runtime_dir, id);
    opts.connect_retry_count = 2;
    opts.connect_retry_delay_ms = 10;
    const auto res = si::StartOrActivate(opts);
    ::CloseHandle(mutex);
    Check(res.status == si::StartStatus::Failed && !res.ShouldContinue(),
          "Expected Failed when mutex exists without ActivateEvent");
  }
#endif
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 4) {
      const std::string mode = argv[1];
      const std::string runtime_dir = argv[2];
      const std::string instance_id = argv[3];
      if (mode == "--child-race") {
        return RunChildRace(runtime_dir, instance_id);
      }
      if (mode == "--child-secondary") {
        return RunChildSecondary(runtime_dir, instance_id);
      }
      if (mode == "--child-crash") {
        return RunChildCrash(runtime_dir, instance_id);
      }
    }

    const std::string self_exe = ResolveSelfExecutable(argc > 0 ? argv[0] : nullptr);
    TempDir temp_dir;

    TestDebugReleaseDefault(temp_dir.Path());
    TestSimultaneousTwoProcesses(self_exe, temp_dir.Path());
    TestActivationBeforeHandlerRegistration(self_exe, temp_dir.Path());
    TestHungClientSocketDoesNotBlock(self_exe, temp_dir.Path());
    TestConcurrentActivationAndShutdown(temp_dir.Path());
    TestRestartAfterAbnormalExit(self_exe, temp_dir.Path());
    TestLockAndIpcInitializationFailures(temp_dir.Path());

    std::cout << "All single-instance tests passed.\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "single_instance_test FAILED: " << ex.what() << '\n';
    return 1;
  }
}
