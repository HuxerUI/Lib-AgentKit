#pragma once

#include <functional>

namespace agentkit_preview::single_instance {

/// Returns true when built as a Debug binary (single-instance enforcement disabled).
bool IsDebugBuild();

/// Returns true when single-instance enforcement is enabled for this build.
bool IsEnabled();

/// Ensures at most one non-Debug desktop instance is running.
/// Returns true if this process may continue starting, or false if an
/// existing instance was signaled to wake its window and this process should exit.
bool StartOrActivate();

/// Registers a callback invoked when a secondary instance requests window activation.
void SetActivationHandler(std::function<void()> handler);

/// Clears the activation callback registered via SetActivationHandler.
void ClearActivationHandler();

/// Stops the background listener and releases instance lock resources.
void Stop();

/// RAII guard that calls Stop() when leaving scope.
struct Guard {
  Guard() = default;
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  ~Guard() { Stop(); }
};

}  // namespace agentkit_preview::single_instance
