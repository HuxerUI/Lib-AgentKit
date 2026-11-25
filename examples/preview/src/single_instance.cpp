#include "single_instance.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
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
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace agentkit_preview::single_instance {

namespace {

// Lock ordering rule: g_lifecycle_mutex may briefly acquire g_handler_mutex,
// never the reverse. Neither mutex is held during thread join or user callbacks.
std::mutex g_lifecycle_mutex;
std::mutex g_handler_mutex;
std::function<void()> g_activation_handler;
bool g_pending_activation = false;

void NotifyActivation() {
  std::function<void()> handler;
  {
    std::lock_guard<std::mutex> lock(g_handler_mutex);
    if (g_activation_handler) {
      handler = g_activation_handler;
    } else {
      g_pending_activation = true;
      return;
    }
  }
  handler();
}

bool ValidateInstanceId(const std::string& instance_id, std::string* error) {
  if (instance_id.empty()) {
    if (error != nullptr) {
      *error = "instance_id must not be empty";
    }
    return false;
  }
  if (instance_id.find('/') != std::string::npos || instance_id.find('\\') != std::string::npos ||
      instance_id.find('\0') != std::string::npos) {
    if (error != nullptr) {
      *error = "instance_id contains invalid path separator or NUL characters";
    }
    return false;
  }
  return true;
}

#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)

std::string FormatErrno(const char* action, int err) {
  return std::string(action) + ": " + std::strerror(err) + " (errno=" + std::to_string(err) + ")";
}

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

std::string ResolveAutoRuntimeDir() {
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

bool ResolveInstancePaths(const Options& options, std::string* lock_path, std::string* sock_path,
                          std::string* error) {
  if (!ValidateInstanceId(options.instance_id, error)) {
    return false;
  }

  const std::string uid_suffix = std::to_string(static_cast<unsigned long>(::getuid()));
  const std::string base = options.instance_id + "-" + uid_suffix;
  const std::string lock_file = base + ".lock";
  const std::string sock_file = base + ".sock";

  const bool explicit_dir = !options.runtime_dir.empty();
  const std::string runtime_dir = explicit_dir ? options.runtime_dir : ResolveAutoRuntimeDir();

  sockaddr_un addr{};
  std::string resolved_lock = JoinPath(runtime_dir, lock_file);
  std::string resolved_sock = JoinPath(runtime_dir, sock_file);

  if (resolved_sock.size() >= sizeof(addr.sun_path)) {
    if (explicit_dir) {
      *error = "socket path exceeds sockaddr_un limit: " + resolved_sock;
      return false;
    }
    resolved_sock = JoinPath("/tmp", sock_file);
    resolved_lock = JoinPath("/tmp", lock_file);
    if (resolved_sock.size() >= sizeof(addr.sun_path)) {
      *error = "fallback socket path exceeds sockaddr_un limit: " + resolved_sock;
      return false;
    }
  }

  *lock_path = std::move(resolved_lock);
  *sock_path = std::move(resolved_sock);
  return true;
}

bool SendAllSocketBytes(int fd, const char* data, size_t len, std::string* error) {
  size_t sent = 0;
  while (sent < len) {
    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    const ssize_t rc = ::send(fd, data + sent, len - sent, flags);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = FormatErrno("send activation message", errno);
      return false;
    }
    if (rc == 0) {
      *error = "send activation message returned 0";
      return false;
    }
    sent += static_cast<size_t>(rc);
  }
  return true;
}

bool TrySendActivationSignal(const std::string& sock_path, int retry_count, int retry_delay_ms,
                             std::string* error) {
  sockaddr_un addr{};
  if (sock_path.size() >= sizeof(addr.sun_path)) {
    *error = "socket path exceeds sockaddr_un limit";
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);

  const int attempts = std::max(1, retry_count);
  const int delay_ms = std::max(1, retry_delay_ms);
  int last_errno = ENOENT;

  for (int attempt = 0; attempt < attempts; ++attempt) {
    UniqueFd sock(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!sock) {
      *error = FormatErrno("socket", errno);
      return false;
    }
    SetCloseOnExec(sock.Get());
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(sock.Get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    ::setsockopt(sock.Get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(sock.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
      static constexpr char kMsg[] = "ACTIVATE\n";
      std::string send_error;
      if (SendAllSocketBytes(sock.Get(), kMsg, sizeof(kMsg) - 1, &send_error)) {
        return true;
      }
      *error = std::move(send_error);
      return false;
    }

    last_errno = errno;
    if (last_errno != ENOENT && last_errno != ECONNREFUSED) {
      *error = FormatErrno("connect", last_errno);
      return false;
    }
    if (attempt + 1 < attempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
  }

  *error = FormatErrno("connect after retries", last_errno);
  return false;
}

UniqueFd g_lock_fd;
UniqueFd g_listen_fd;
UniqueFd g_wakeup_read_fd;
UniqueFd g_wakeup_write_fd;
std::string g_sock_path;
std::thread g_listener_thread;
bool g_started = false;

struct ClientReadOutcome {
  bool activated = false;
  bool stop_requested = false;
};

ClientReadOutcome ReadClientActivation(int wakeup_fd, int client_fd, int read_timeout_ms) {
  static constexpr std::string_view kExpected = "ACTIVATE\n";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(10, read_timeout_ms));

  std::string buffer;
  buffer.reserve(64);

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return {};
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int poll_ms = static_cast<int>(std::max<long long>(1, remaining));

    pollfd fds[2]{};
    fds[0].fd = wakeup_fd;
    fds[0].events = POLLIN;
    fds[1].fd = client_fd;
    fds[1].events = POLLIN;

    const int rc = ::poll(fds, 2, poll_ms);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return {};
    }
    if (rc == 0) {
      return {};
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      return {false, true};
    }
    if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
      char chunk[64];
      for (;;) {
        const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
          const size_t take = std::min(static_cast<size_t>(n), 64 - buffer.size());
          buffer.append(chunk, take);
          if (buffer.find(kExpected) != std::string::npos) {
            return {true, false};
          }
          if (buffer.size() >= 64) {
            return {};
          }
          continue;
        }
        if (n == 0) {
          return {buffer.find(kExpected) != std::string::npos, false};
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        return {};
      }
      if ((fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return {buffer.find(kExpected) != std::string::npos, false};
      }
    }
  }
}

void RunPosixListener(int wakeup_fd, int listen_fd, int read_timeout_ms) {
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
        SetNonBlocking(client.Get());

        const ClientReadOutcome outcome =
            ReadClientActivation(wakeup_fd, client.Get(), read_timeout_ms);
        client.Reset();
        if (outcome.stop_requested) {
          return;
        }
        if (outcome.activated) {
          NotifyActivation();
        }
      }
    }
    if ((fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      break;
    }
  }
}

void StopPosixLocked() {
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

StartResult StartOrActivatePosix(const Options& options) {
  std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mutex);
  if (g_started) {
    return {StartStatus::Acquired, {}};
  }
  {
    std::lock_guard<std::mutex> handler_lock(g_handler_mutex);
    g_pending_activation = false;
  }

  std::string lock_path;
  std::string sock_path;
  std::string path_error;
  if (!ResolveInstancePaths(options, &lock_path, &sock_path, &path_error)) {
    return {StartStatus::Failed, std::move(path_error)};
  }

  UniqueFd lock_fd(::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600));
  if (!lock_fd) {
    return {StartStatus::Failed, FormatErrno("open lock file", errno)};
  }
  SetCloseOnExec(lock_fd.Get());

  if (::flock(lock_fd.Get(), LOCK_EX | LOCK_NB) != 0) {
    const int flock_err = errno;
    if (flock_err == EWOULDBLOCK || flock_err == EAGAIN) {
      std::string send_error;
      if (TrySendActivationSignal(sock_path, options.connect_retry_count,
                                  options.connect_retry_delay_ms, &send_error)) {
        return {StartStatus::ActivatedExisting, {}};
      }
      return {StartStatus::Failed,
              "existing instance holds lock, but failed to signal activation: " + send_error};
    }
    return {StartStatus::Failed, FormatErrno("flock", flock_err)};
  }

  int pipe_fds[2]{-1, -1};
  if (::pipe(pipe_fds) != 0) {
    return {StartStatus::Failed, FormatErrno("pipe", errno)};
  }
  UniqueFd wakeup_read_fd(pipe_fds[0]);
  UniqueFd wakeup_write_fd(pipe_fds[1]);
  SetCloseOnExec(wakeup_read_fd.Get());
  SetCloseOnExec(wakeup_write_fd.Get());
  SetNonBlocking(wakeup_read_fd.Get());
  SetNonBlocking(wakeup_write_fd.Get());

  UniqueFd listen_fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listen_fd) {
    return {StartStatus::Failed, FormatErrno("socket", errno)};
  }
  SetCloseOnExec(listen_fd.Get());
  SetNonBlocking(listen_fd.Get());

  if (::unlink(sock_path.c_str()) != 0 && errno != ENOENT) {
    return {StartStatus::Failed, FormatErrno("unlink stale socket", errno)};
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);

  if (::bind(listen_fd.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return {StartStatus::Failed, FormatErrno("bind socket", errno)};
  }

  if (::listen(listen_fd.Get(), 8) != 0) {
    const int err = errno;
    ::unlink(sock_path.c_str());
    return {StartStatus::Failed, FormatErrno("listen socket", err)};
  }

  try {
    const int wakeup_fd = wakeup_read_fd.Get();
    const int accept_fd = listen_fd.Get();
    const int read_timeout_ms = options.client_read_timeout_ms;
    g_listener_thread = std::thread([wakeup_fd, accept_fd, read_timeout_ms]() {
      RunPosixListener(wakeup_fd, accept_fd, read_timeout_ms);
    });
  } catch (const std::exception& ex) {
    ::unlink(sock_path.c_str());
    return {StartStatus::Failed, std::string("start listener thread: ") + ex.what()};
  } catch (...) {
    ::unlink(sock_path.c_str());
    return {StartStatus::Failed, "start listener thread failed"};
  }

  g_lock_fd = std::move(lock_fd);
  g_wakeup_read_fd = std::move(wakeup_read_fd);
  g_wakeup_write_fd = std::move(wakeup_write_fd);
  g_listen_fd = std::move(listen_fd);
  g_sock_path = std::move(sock_path);
  g_started = true;
  return {StartStatus::Acquired, {}};
}

#elif defined(_WIN32)

std::string FormatWin32Error(const char* action, DWORD err) {
  return std::string(action) + " failed (error=" + std::to_string(err) + ")";
}

bool Utf8ToWide(const std::string& input, std::wstring* output) {
  if (input.empty()) {
    output->clear();
    return true;
  }
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (needed <= 0) {
    return false;
  }
  output->assign(static_cast<size_t>(needed), L'\0');
  const int converted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                              static_cast<int>(input.size()), output->data(), needed);
  return converted == needed;
}

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueHandle() { Reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  HANDLE Get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

  HANDLE Release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  void Reset(HANDLE new_handle = nullptr) {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
    handle_ = new_handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

struct Win32Names {
  std::wstring mutex_name;
  std::wstring event_name;
  std::wstring pid_map_name;
};

bool BuildWin32Names(const std::string& instance_id, Win32Names* names, std::string* error) {
  if (!ValidateInstanceId(instance_id, error)) {
    return false;
  }
  std::wstring wide_id;
  if (!Utf8ToWide(instance_id, &wide_id)) {
    *error = "instance_id is not valid UTF-8";
    return false;
  }
  const std::wstring prefix = L"Local\\HuxerUI.GanWork." + wide_id;
  names->mutex_name = prefix + L".SingleInstance";
  names->event_name = prefix + L".ActivateEvent";
  names->pid_map_name = prefix + L".PrimaryPid";
  return true;
}

DWORD ReadPrimaryPid(const std::wstring& pid_map_name) {
  UniqueHandle mapping(::OpenFileMappingW(FILE_MAP_READ, FALSE, pid_map_name.c_str()));
  if (!mapping) {
    return 0;
  }
  const void* view = ::MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, sizeof(DWORD));
  if (view == nullptr) {
    return 0;
  }
  DWORD pid = 0;
  std::memcpy(&pid, view, sizeof(DWORD));
  ::UnmapViewOfFile(view);
  return pid;
}

UniqueHandle g_mutex;
UniqueHandle g_activate_event;
UniqueHandle g_stop_event;
UniqueHandle g_pid_mapping;
std::thread g_listener_thread;
bool g_started = false;

void StopWin32Locked() {
  if (!g_started) {
    return;
  }
  if (g_stop_event) {
    ::SetEvent(g_stop_event.Get());
  }
  if (g_listener_thread.joinable()) {
    g_listener_thread.join();
  }
  g_pid_mapping.Reset();
  g_stop_event.Reset();
  g_activate_event.Reset();
  g_mutex.Reset();
  g_started = false;
}

StartResult StartOrActivateWin32(const Options& options) {
  std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mutex);
  if (g_started) {
    return {StartStatus::Acquired, {}};
  }
  {
    std::lock_guard<std::mutex> handler_lock(g_handler_mutex);
    g_pending_activation = false;
  }

  Win32Names names;
  std::string name_error;
  if (!BuildWin32Names(options.instance_id, &names, &name_error)) {
    return {StartStatus::Failed, std::move(name_error)};
  }

  UniqueHandle mutex(::CreateMutexW(nullptr, FALSE, names.mutex_name.c_str()));
  const DWORD mutex_err = ::GetLastError();
  if (!mutex) {
    return {StartStatus::Failed, FormatWin32Error("CreateMutexW", mutex_err)};
  }

  if (mutex_err == ERROR_ALREADY_EXISTS) {
    mutex.Reset();
    const int attempts = std::max(1, options.connect_retry_count);
    const int delay_ms = std::max(1, options.connect_retry_delay_ms);
    DWORD last_open_err = ERROR_FILE_NOT_FOUND;

    for (int attempt = 0; attempt < attempts; ++attempt) {
      UniqueHandle activate_evt(::OpenEventW(EVENT_MODIFY_STATE, FALSE, names.event_name.c_str()));
      if (activate_evt) {
        const DWORD primary_pid = ReadPrimaryPid(names.pid_map_name);
        ::AllowSetForegroundWindow(primary_pid != 0 ? primary_pid : ASFW_ANY);
        if (::SetEvent(activate_evt.Get()) != 0) {
          return {StartStatus::ActivatedExisting, {}};
        }
        return {StartStatus::Failed, FormatWin32Error("SetEvent", ::GetLastError())};
      }
      last_open_err = ::GetLastError();
      if (attempt + 1 < attempts) {
        ::Sleep(static_cast<DWORD>(delay_ms));
      }
    }
    return {StartStatus::Failed,
            "existing instance holds mutex, but failed to open activation event: " +
                FormatWin32Error("OpenEventW", last_open_err)};
  }

  UniqueHandle activate_event(::CreateEventW(nullptr, FALSE, FALSE, names.event_name.c_str()));
  if (!activate_event) {
    return {StartStatus::Failed, FormatWin32Error("CreateEventW(activate)", ::GetLastError())};
  }

  UniqueHandle stop_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!stop_event) {
    return {StartStatus::Failed, FormatWin32Error("CreateEventW(stop)", ::GetLastError())};
  }

  UniqueHandle pid_mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              sizeof(DWORD), names.pid_map_name.c_str()));
  if (pid_mapping) {
    void* view = ::MapViewOfFile(pid_mapping.Get(), FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
    if (view != nullptr) {
      const DWORD pid = ::GetCurrentProcessId();
      std::memcpy(view, &pid, sizeof(DWORD));
      ::UnmapViewOfFile(view);
    }
  }

  try {
    HANDLE stop_handle = stop_event.Get();
    HANDLE activate_handle = activate_event.Get();
    g_listener_thread = std::thread([stop_handle, activate_handle]() {
      HANDLE handles[2] = {stop_handle, activate_handle};
      for (;;) {
        const DWORD wait_rc = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait_rc == WAIT_OBJECT_0 + 1) {
          NotifyActivation();
          continue;
        }
        break;
      }
    });
  } catch (const std::exception& ex) {
    return {StartStatus::Failed, std::string("start listener thread: ") + ex.what()};
  } catch (...) {
    return {StartStatus::Failed, "start listener thread failed"};
  }

  g_mutex = std::move(mutex);
  g_activate_event = std::move(activate_event);
  g_stop_event = std::move(stop_event);
  g_pid_mapping = std::move(pid_mapping);
  g_started = true;
  return {StartStatus::Acquired, {}};
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

StartResult StartOrActivate(const Options& options) {
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP) || defined(_WIN32)
  if (!options.force_enable && !IsEnabled()) {
    return {StartStatus::Disabled, {}};
  }
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
  return StartOrActivatePosix(options);
#elif defined(_WIN32)
  return StartOrActivateWin32(options);
#endif
#else
  (void)options;
  return {StartStatus::Disabled, {}};
#endif
}

void SetActivationHandler(std::function<void()> handler) {
  std::function<void()> pending_to_run;
  {
    std::lock_guard<std::mutex> lock(g_handler_mutex);
    g_activation_handler = std::move(handler);
    if (g_activation_handler && g_pending_activation) {
      g_pending_activation = false;
      pending_to_run = g_activation_handler;
    }
  }
  if (pending_to_run) {
    pending_to_run();
  }
}

void ClearActivationHandler() {
  std::lock_guard<std::mutex> lock(g_handler_mutex);
  g_activation_handler = nullptr;
}

void Stop() {
  std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mutex);
  {
    std::lock_guard<std::mutex> handler_lock(g_handler_mutex);
    g_activation_handler = nullptr;
    g_pending_activation = false;
  }
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
  StopPosixLocked();
#elif defined(_WIN32)
  StopWin32Locked();
#endif
  {
    std::lock_guard<std::mutex> handler_lock(g_handler_mutex);
    g_activation_handler = nullptr;
    g_pending_activation = false;
  }
}

}  // namespace agentkit_preview::single_instance
