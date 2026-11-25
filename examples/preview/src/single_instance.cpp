#include "single_instance.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// Lock ordering rule:
// - g_lifecycle_mutex may briefly acquire g_handler_mutex, never the reverse.
// - Neither g_lifecycle_mutex nor g_handler_mutex is ever held while joining
//   the listener thread or while invoking user activation callbacks.
std::mutex g_lifecycle_mutex;
std::condition_variable g_lifecycle_cv;
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

#endif

#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP) || defined(_WIN32)

struct InstanceSession {
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
  UniqueFd lock_fd;
  UniqueFd listen_fd;
  UniqueFd wakeup_read_fd;
  UniqueFd wakeup_write_fd;
  std::string sock_path;
#elif defined(_WIN32)
  UniqueHandle mutex;
  UniqueHandle activate_event;
  UniqueHandle stop_event;
  UniqueHandle pid_mapping;
#endif
  std::thread listener_thread;
  std::thread::id listener_tid{};
  std::atomic<bool> stop_requested{false};
  bool self_stopped = false;
  bool join_claimed = false;

  void SignalStop() {
    if (stop_requested.exchange(true)) {
      return;
    }
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
    if (wakeup_write_fd) {
      const char byte = 1;
      while (::write(wakeup_write_fd.Get(), &byte, 1) < 0 && errno == EINTR) {
      }
    }
#elif defined(_WIN32)
    if (stop_event) {
      ::SetEvent(stop_event.Get());
    }
#endif
  }

  void CloseResources() {
#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)
    listen_fd.Reset();
    wakeup_read_fd.Reset();
    wakeup_write_fd.Reset();
    if (!sock_path.empty()) {
      ::unlink(sock_path.c_str());
      sock_path.clear();
    }
    lock_fd.Reset();
#elif defined(_WIN32)
    pid_mapping.Reset();
    stop_event.Reset();
    activate_event.Reset();
    mutex.Reset();
#endif
  }
};

std::shared_ptr<InstanceSession> g_active_session;
std::vector<std::shared_ptr<InstanceSession>> g_retiring_sessions;

void EraseRetiringSessionLocked(const std::shared_ptr<InstanceSession>& session) {
  auto it = std::find(g_retiring_sessions.begin(), g_retiring_sessions.end(), session);
  if (it != g_retiring_sessions.end()) {
    g_retiring_sessions.erase(it);
  }
}

bool DrainRetiringSessions(std::unique_lock<std::mutex>& lock) {
  while (!g_retiring_sessions.empty()) {
    const auto self_tid = std::this_thread::get_id();
    for (const auto& s : g_retiring_sessions) {
      if (s->listener_tid == self_tid) {
        return false;
      }
    }

    std::shared_ptr<InstanceSession> to_join;
    for (const auto& s : g_retiring_sessions) {
      if (!s->join_claimed) {
        s->join_claimed = true;
        to_join = s;
        break;
      }
    }

    if (to_join) {
      lock.unlock();
      if (to_join->listener_thread.joinable()) {
        to_join->listener_thread.join();
      }
      to_join->CloseResources();
      lock.lock();
      EraseRetiringSessionLocked(to_join);
      g_lifecycle_cv.notify_all();
      continue;
    }

    g_lifecycle_cv.wait(lock);
  }
  return true;
}

void FinalizeSessionOnThreadExit(const std::shared_ptr<InstanceSession>& session) {
  bool should_cleanup = false;
  {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (session->self_stopped && !session->join_claimed) {
      session->join_claimed = true;
      if (session->listener_thread.joinable()) {
        session->listener_thread.detach();
      }
      should_cleanup = true;
    }
  }
  if (!should_cleanup) {
    return;
  }
  session->CloseResources();
  {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    EraseRetiringSessionLocked(session);
  }
  g_lifecycle_cv.notify_all();
}

#endif

#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP)

struct ClientReadOutcome {
  bool activated = false;
  bool stop_requested = false;
};

ClientReadOutcome ReadClientActivation(const std::shared_ptr<InstanceSession>& session,
                                       int wakeup_fd, int client_fd, int read_timeout_ms) {
  static constexpr std::string_view kExpected = "ACTIVATE\n";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(10, read_timeout_ms));

  std::string buffer;
  buffer.reserve(64);

  for (;;) {
    if (session->stop_requested.load()) {
      return {false, true};
    }
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
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 || session->stop_requested.load()) {
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

void RunPosixListener(const std::shared_ptr<InstanceSession>& session, int read_timeout_ms) {
  const int wakeup_fd = session->wakeup_read_fd.Get();
  const int listen_fd = session->listen_fd.Get();

  pollfd fds[2]{};
  fds[0].fd = wakeup_fd;
  fds[0].events = POLLIN;
  fds[1].fd = listen_fd;
  fds[1].events = POLLIN;

  while (!session->stop_requested.load()) {
    const int rc = ::poll(fds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 || session->stop_requested.load()) {
      break;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      for (;;) {
        if (session->stop_requested.load()) {
          return;
        }
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
            ReadClientActivation(session, wakeup_fd, client.Get(), read_timeout_ms);
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

StartResult StartOrActivatePosix(const Options& options) {
  std::unique_lock<std::mutex> lifecycle_lock(g_lifecycle_mutex);
  if (g_active_session) {
    return {StartStatus::Acquired, {}};
  }
  if (!DrainRetiringSessions(lifecycle_lock)) {
    return {StartStatus::Failed,
            "StartOrActivate cannot be called reentrantly from an activation callback during shutdown"};
  }
  if (g_active_session) {
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

  auto session = std::make_shared<InstanceSession>();
  session->lock_fd.Reset(::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600));
  if (!session->lock_fd) {
    return {StartStatus::Failed, FormatErrno("open lock file", errno)};
  }
  SetCloseOnExec(session->lock_fd.Get());

  if (::flock(session->lock_fd.Get(), LOCK_EX | LOCK_NB) != 0) {
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
  session->wakeup_read_fd.Reset(pipe_fds[0]);
  session->wakeup_write_fd.Reset(pipe_fds[1]);
  SetCloseOnExec(session->wakeup_read_fd.Get());
  SetCloseOnExec(session->wakeup_write_fd.Get());
  SetNonBlocking(session->wakeup_read_fd.Get());
  SetNonBlocking(session->wakeup_write_fd.Get());

  session->listen_fd.Reset(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!session->listen_fd) {
    return {StartStatus::Failed, FormatErrno("socket", errno)};
  }
  SetCloseOnExec(session->listen_fd.Get());
  SetNonBlocking(session->listen_fd.Get());

  if (::unlink(sock_path.c_str()) != 0 && errno != ENOENT) {
    return {StartStatus::Failed, FormatErrno("unlink stale socket", errno)};
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);

  if (::bind(session->listen_fd.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return {StartStatus::Failed, FormatErrno("bind socket", errno)};
  }
  session->sock_path = std::move(sock_path);

  if (::listen(session->listen_fd.Get(), 8) != 0) {
    const int err = errno;
    session->CloseResources();
    return {StartStatus::Failed, FormatErrno("listen socket", err)};
  }

  try {
    const int read_timeout_ms = options.client_read_timeout_ms;
    session->listener_thread = std::thread([session, read_timeout_ms]() {
      RunPosixListener(session, read_timeout_ms);
      FinalizeSessionOnThreadExit(session);
    });
    session->listener_tid = session->listener_thread.get_id();
  } catch (const std::exception& ex) {
    session->CloseResources();
    return {StartStatus::Failed, std::string("start listener thread: ") + ex.what()};
  } catch (...) {
    session->CloseResources();
    return {StartStatus::Failed, "start listener thread failed"};
  }

  g_active_session = std::move(session);
  return {StartStatus::Acquired, {}};
}

#elif defined(_WIN32)

void RunWin32Listener(const std::shared_ptr<InstanceSession>& session) {
  HANDLE handles[2] = {session->stop_event.Get(), session->activate_event.Get()};
  while (!session->stop_requested.load()) {
    const DWORD wait_rc = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (wait_rc == WAIT_OBJECT_0 + 1 && !session->stop_requested.load()) {
      NotifyActivation();
      continue;
    }
    break;
  }
}

StartResult StartOrActivateWin32(const Options& options) {
  std::unique_lock<std::mutex> lifecycle_lock(g_lifecycle_mutex);
  if (g_active_session) {
    return {StartStatus::Acquired, {}};
  }
  if (!DrainRetiringSessions(lifecycle_lock)) {
    return {StartStatus::Failed,
            "StartOrActivate cannot be called reentrantly from an activation callback during shutdown"};
  }
  if (g_active_session) {
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

  auto session = std::make_shared<InstanceSession>();
  session->mutex.Reset(::CreateMutexW(nullptr, FALSE, names.mutex_name.c_str()));
  const DWORD mutex_err = ::GetLastError();
  if (!session->mutex) {
    return {StartStatus::Failed, FormatWin32Error("CreateMutexW", mutex_err)};
  }

  if (mutex_err == ERROR_ALREADY_EXISTS) {
    session->mutex.Reset();
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

  session->activate_event.Reset(::CreateEventW(nullptr, FALSE, FALSE, names.event_name.c_str()));
  if (!session->activate_event) {
    return {StartStatus::Failed, FormatWin32Error("CreateEventW(activate)", ::GetLastError())};
  }

  session->stop_event.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!session->stop_event) {
    return {StartStatus::Failed, FormatWin32Error("CreateEventW(stop)", ::GetLastError())};
  }

  session->pid_mapping.Reset(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                  sizeof(DWORD), names.pid_map_name.c_str()));
  if (session->pid_mapping) {
    void* view = ::MapViewOfFile(session->pid_mapping.Get(), FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
    if (view != nullptr) {
      const DWORD pid = ::GetCurrentProcessId();
      std::memcpy(view, &pid, sizeof(DWORD));
      ::UnmapViewOfFile(view);
    }
  }

  try {
    session->listener_thread = std::thread([session]() {
      RunWin32Listener(session);
      FinalizeSessionOnThreadExit(session);
    });
    session->listener_tid = session->listener_thread.get_id();
  } catch (const std::exception& ex) {
    session->CloseResources();
    return {StartStatus::Failed, std::string("start listener thread: ") + ex.what()};
  } catch (...) {
    session->CloseResources();
    return {StartStatus::Failed, "start listener thread failed"};
  }

  g_active_session = std::move(session);
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
  std::unique_lock<std::mutex> lifecycle_lock(g_lifecycle_mutex);
  {
    std::lock_guard<std::mutex> handler_lock(g_handler_mutex);
    g_activation_handler = nullptr;
    g_pending_activation = false;
  }

#if defined(AGENTKIT_PREVIEW_POSIX_DESKTOP) || defined(_WIN32)
  if (g_active_session) {
    auto session = std::move(g_active_session);
    const bool is_self = (session->listener_tid == std::this_thread::get_id());
    session->SignalStop();
    if (is_self) {
      session->self_stopped = true;
      g_retiring_sessions.push_back(std::move(session));
      return;
    }
    g_retiring_sessions.push_back(std::move(session));
  }

  if (!DrainRetiringSessions(lifecycle_lock)) {
    return;
  }

  if (!g_active_session) {
    std::lock_guard<std::mutex> handler_lock(g_handler_mutex);
    g_activation_handler = nullptr;
    g_pending_activation = false;
  }
#endif
}

}  // namespace agentkit_preview::single_instance
