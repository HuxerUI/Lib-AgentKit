#pragma once

#include <functional>
#include <string>

namespace agentkit_preview::single_instance {

enum class StartStatus {
  Disabled,
  Acquired,
  ActivatedExisting,
  Failed,
};

struct StartResult {
  StartStatus status = StartStatus::Failed;
  std::string error;

  bool ShouldContinue() const {
    return status == StartStatus::Disabled || status == StartStatus::Acquired;
  }
};

struct Options {
  std::string instance_id = "ganwork";
  std::string runtime_dir;
  bool force_enable = false;
  int client_read_timeout_ms = 100;
  int connect_retry_count = 10;
  int connect_retry_delay_ms = 25;
};

/// Returns true when built as a Debug binary (single-instance disabled by default).
bool IsDebugBuild();

/// Returns true when single-instance enforcement is enabled by default for this build.
bool IsEnabled();

/// Ensures at most one active desktop instance runs for the given instance_id.
StartResult StartOrActivate(const Options& options = {});

/// Registers a callback invoked on a background listener thread when a secondary
/// instance requests window activation. If an activation arrived before registration,
/// it is delivered synchronously on the calling thread.
/// Reentrant calls to Stop(), ClearActivationHandler(), or SetActivationHandler()
/// from within the callback are safe and deadlock-free; calling StartOrActivate()
/// reentrantly while shutdown is in progress returns StartStatus::Failed.
void SetActivationHandler(std::function<void()> handler);

/// Clears the activation callback registered via SetActivationHandler.
void ClearActivationHandler();

/// Stops the background listener and releases instance lock resources.
/// Safe to call concurrently or reentrantly from an activation callback.
void Stop();

/// RAII guard that calls Stop() when leaving scope.
struct Guard {
  Guard() = default;
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  ~Guard() { Stop(); }
};

}  // namespace agentkit_preview::single_instance
