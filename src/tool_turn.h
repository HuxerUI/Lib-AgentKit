#pragma once

#include "runtime_internal.h"

namespace huxerui::agentkit::detail {

struct ToolTurnSchedule : std::enable_shared_from_this<ToolTurnSchedule> {
  enum class Phase { Preparing, Ready, Admitted, Done };

  struct Slot {
    ToolCallId call;
    Phase phase = Phase::Preparing;
    std::optional<std::vector<ToolResourceClaim>> claims;
    std::optional<CallbackCompletion<bool>> ready;
    TaskHandle task;
  };

  explicit ToolTurnSchedule(std::size_t limit) : limit(limit) {}

  Task<bool> Acquire(ToolCallId call, std::optional<std::vector<ToolResourceClaim>> claims);

  void Release(const ToolCallId& call);

  void Finish(const ToolCallId& call);

  void Abort(const AgentError& reason);

  Task<void> Drain();

  std::vector<Slot> slots;
  const std::size_t limit;
  std::optional<AgentError> failure;

private:
  Slot& Find(const ToolCallId& call);

  void AdmitReady();

  bool Done() const;
  std::optional<CallbackCompletion<bool>> drained;
};

} // namespace huxerui::agentkit::detail
