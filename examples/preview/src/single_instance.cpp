#include "single_instance.h"

#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#define AGENTKIT_PREVIEW_POSIX_DESKTOP 1
#elif defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
#define AGENTKIT_PREVIEW_POSIX_DESKTOP 1
#endif
#endif

#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace agentkit_preview::single_instance {

namespace {

std::mutex g_state_mutex;
std::function<void()> g_activation_handler;

void InvokeActivationHandler() {
  std::function<void()> handler;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    handler = g_activation_handler;
  }
  if (handler) {
    handler();
  }
}

#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  int Get() const { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }

  int Release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void Reset(int new_fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

 private:
  int fd_ = -1;
};

void SetCloseOnExec(int fd) {
  if (fd >= 0) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
      ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
}

void SetNonBlocking(int fd) {
  if (fd >= 0) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }
}

std::string ResolveRuntimeDir() {
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && xdg[0] != '\0') {
    return std::string(xdg);
  }
  if (const char* tmpdir = std::getenv("TMPDIR"); tmpdir != nullptr && tmpdir[0] != '\0') {
    return std::string(tmpdir);
  }
  return "/tmp";
}

std::string JoinPath(const std::string& dir, const std::string& file) {
  if (!dir.empty() && dir.back() == '/') {
    return dir + file;
  }
  return dir + "/" + file;
}

std::pair<std::string, std::string> ResolveInstancePaths() {
  const std::string suffix = std::to_string(static_cast<unsigned long>(::getuid()));
  const std::string runtime_dir = ResolveRuntimeDir();
  const std::string lock_file = "ganwork-" + suffix + ".lock";
  const std::string sock_file = "ganwork-" + suffix + ".sock";

  std::string sock_path = JoinPath(runtime_dir, sock_file);
  sockaddr_un addr{};
  if (sock_path.size() >= sizeof(addr.sun_path)) {
    sock_path = JoinPath("/tmp", sock_file);
  }
  std::string lock_path = JoinPath(runtime_dir, lock_file);
  if (lock_path.size() >= 256) {
    lock_path = JoinPath("/tmp", lock_file);
  }
  return {lock_path, sock_path};
}

bool TrySendActivationSignal(const std::string& sock_path) {
  sockaddr_un addr{};
  if (sock_path.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);

  for (int attempt = 0; attempt < 10; ++attempt) {
    UniqueFd sock(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!sock) {
      return false;
    }
    SetCloseOnExec(sock.Get());
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(sock.Get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (::connect(sock.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
      static constexpr char kMsg[] = "ACTIVATE\n";
      int flags = 0;
#if defined(MSG_NOSIGNAL)
      flags = MSG_NOSIGNAL;
#endif
      ::send(sock.Get(), kMsg, sizeof(kMsg) - 1, flags);
      return true;
    }
    if (errno != ENOENT && errno != ECONNREFUSED) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

UniqueFd g_lock_fd;
UniqueFd g_listen_fd;
UniqueFd g_wakeup_read_fd;
UniqueFd g_wakeup_write_fd;
std::string g_sock_path;
std::thread g_listener_thread;
bool g_started = false;

void StopPosixUnlocked() {
  if (!g_started) {
    return;
  }
  if (g_wakeup_write_fd) {
    const char byte = 1;
    while (::write(g_wakeup_write_fd.Get(), &byte, 1) < 0 && errno == EINTR) {
    }
  }
  if (g_listener_thread.joinable()) {
    g_listener_thread.join();
  }
  g_listen_fd.Reset();
  g_wakeup_read_fd.Reset();
  g_wakeup_write_fd.Reset();
  if (!g_sock_path.empty()) {
    ::unlink(g_sock_path.c_str());
    g_sock_path.clear();
  }
  g_lock_fd.Reset();
  g_started = false;
}

void RunPosixListener(int wakeup_fd, int listen_fd) {
  pollfd fds[2]{};
  fds[0].fd = wakeup_fd;
  fds[0].events = POLLIN;
  fds[1].fd = listen_fd;
  fds[1].events = POLLIN;

  for (;;) {
    const int rc = ::poll(fds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      break;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      for (;;) {
        UniqueFd client(::accept(listen_fd, nullptr, nullptr));
        if (!client) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
          }
          return;
        }
        SetCloseOnExec(client.Get());
        char buf[64];
        while (::recv(client.Get(), buf, sizeof(buf), 0) > 0) {
        }
        InvokeActivationHandler();
      }
    }
    if ((fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      break;
    }
  }
}

bool StartOrActivatePosix() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_started) {
    return true;
  }

  const auto [lock_path, sock_path] = ResolveInstancePaths();
  UniqueFd lock_fd(::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600));
  if (!lock_fd) {
    return true;
  }
  SetCloseOnExec(lock_fd.Get());

  if (::flock(lock_fd.Get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      TrySendActivationSignal(sock_path);
      return false;
    }
    return true;
  }

  int pipe_fds[2]{-1, -1};
  if (::pipe(pipe_fds) != 0) {
    g_lock_fd = std::move(lock_fd);
    g_started = true;
    return true;
  }
  SetCloseOnExec(pipe_fds[0]);
  SetCloseOnExec(pipe_fds[1]);
  SetNonBlocking(pipe_fds[0]);
  SetNonBlocking(pipe_fds[1]);

  UniqueFd listen_fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listen_fd) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    g_lock_fd = std::move(lock_fd);
    g_started = true;
    return true;
  }
  SetCloseOnExec(listen_fd.Get());
  SetNonBlocking(listen_fd.Get());

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);
  ::unlink(sock_path.c_str());

  if (::bind(listen_fd.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd.Get(), 8) != 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    g_lock_fd = std::move(lock_fd);
    g_started = true;
    return true;
  }

  g_lock_fd = std::move(lock_fd);
  g_wakeup_read_fd.Reset(pipe_fds[0]);
  g_wakeup_write_fd.Reset(pipe_fds[1]);
  g_listen_fd = std::move(listen_fd);
  g_sock_path = sock_path;
  g_started = true;

  const int wakeup_fd = g_wakeup_read_fd.Get();
  const int accept_fd = g_listen_fd.Get();
  g_listener_thread = std::thread([wakeup_fd, accept_fd]() {
    RunPosixListener(wakeup_fd, accept_fd);
  });
  return true;
}

#elif defined(_WIN32)

constexpr wchar_t kMutexName[] = L"Local\\HuxerUI.GanWork.SingleInstance";
constexpr wchar_t kEventName[] = L"Local\\HuxerUI.GanWork.ActivateEvent";

HANDLE g_mutex = nullptr;
HANDLE g_activate_event = nullptr;
HANDLE g_stop_event = nullptr;
std::thread g_listener_thread;
bool g_started = false;

void StopWin32Unlocked() {
  if (!g_started) {
    return;
  }
  if (g_stop_event != nullptr) {
    ::SetEvent(g_stop_event);
  }
  if (g_listener_thread.joinable()) {
    g_listener_thread.join();
  }
  if (g_stop_event != nullptr) {
    ::CloseHandle(g_stop_event);
    g_stop_event = nullptr;
  }
  if (g_activate_event != nullptr) {
    ::CloseHandle(g_activate_event);
    g_activate_event = nullptr;
  }
  if (g_mutex != nullptr) {
    ::CloseHandle(g_mutex);
    g_mutex = nullptr;
  }
  g_started = false;
}

bool StartOrActivateWin32() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_started) {
    return true;
  }

  HANDLE mutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
  const DWORD last_error = ::GetLastError();
  if (mutex == nullptr) {
    return true;
  }

  if (last_error == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(mutex);
    bool signaled = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
      HANDLE activate_evt = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kEventName);
      if (activate_evt != nullptr) {
        ::AllowSetForegroundWindow(ASFW_ANY);
        ::SetEvent(activate_evt);
        ::CloseHandle(activate_evt);
        signaled = true;
        break;
      }
      ::Sleep(50);
    }
    if (!signaled) {
      if (HWND hwnd = ::FindWindowW(nullptr, L"GanWork"); hwnd != nullptr) {
        if (::IsIconic(hwnd)) {
          ::ShowWindow(hwnd, SW_RESTORE);
        }
        ::SetForegroundWindow(hwnd);
      }
    }
    return false;
  }

  g_mutex = mutex;
  g_activate_event = ::CreateEventW(nullptr, FALSE, FALSE, kEventName);
  g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_started = true;

  if (g_activate_event != nullptr && g_stop_event != nullptr) {
    HANDLE stop_evt = g_stop_event;
    HANDLE activate_evt = g_activate_event;
    g_listener_thread = std::thread([stop_evt, activate_evt]() {
      HANDLE handles[2] = {stop_evt, activate_evt};
      for (;;) {
        const DWORD wait_rc = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait_rc == WAIT_OBJECT_0 + 1) {
          InvokeActivationHandler();
          continue;
        }
        break;
      }
    });
  }
  return true;
}

#endif

}  // namespace

bool IsDebugBuild() {
#if defined(AGENTKIT_PREVIEW_DEBUG) || !defined(NDEBUG)
  return true;
#else
  return false;
#endif
}

bool IsEnabled() {
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP) || defined(_WIN32)
  return !IsDebugBuild();
#else
  return false;
#endif
}

bool StartOrActivate() {
  if (!IsEnabled()) {
    return true;
  }
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
  return StartOrActivatePosix();
#elif defined(_WIN32)
  return StartOrActivateWin32();
#else
  return true;
#endif
}

void SetActivationHandler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_activation_handler = std::move(handler);
}

void ClearActivationHandler() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_activation_handler = nullptr;
}

void Stop() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
  StopPosixUnlocked();
#elif defined(_WIN32)
  StopWin32Unlocked();
#endif
  g_activation_handler = nullptr;
}

}  // namespace agentkit_preview::single_instance
