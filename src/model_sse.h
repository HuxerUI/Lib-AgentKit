#pragma once

#include <huxerui/agentkit/model.h>

#include <deque>

namespace huxerui::agentkit::detail {

struct SseFrame {
  std::string type;
  std::string data;
};

class SseModelProtocol {
public:
  virtual ~SseModelProtocol() = default;

  virtual AgentResult<bool> Accept(SseFrame frame, std::deque<ModelEvent>& events) = 0;

  virtual AgentResult<void> End(std::deque<ModelEvent>& events);

  // The protocol has seen a terminal signal and only optional trailing information remains.
  virtual bool HasTerminalFrame() const noexcept { return false; }
};

using ModelClock = std::chrono::steady_clock;

enum class ModelTimeout { FirstResponse, Idle, Total };

AgentError ModelTimeoutError(ModelTimeout timeout);

struct ModelStreamDeadlines {
  ModelClock::time_point first;
  ModelClock::time_point total;
  Duration idle;
};

template<class T>
Task<AgentResult<T>> WaitForModelIo(TaskScope tasks, Task<AgentResult<T>> io, std::stop_token stop,
    ModelClock::time_point deadline, AgentError timed_out) {
  using Result = AgentResult<T>;
  struct Pending {
    TaskHandle operation;
    TaskHandle timer;
    std::optional<std::stop_callback<std::function<void()>>> on_stop;

    void Cancel() noexcept {
      operation.Cancel();
      timer.Cancel();
    }

    ~Pending() { Cancel(); }
  };
  const auto cancelled = [] {
    return Result::Failure({.code = ErrorCode::AlreadyClosed, .message = "Model turn cancelled"});
  };
  if (stop.stop_requested()) co_return cancelled();
  if (ModelClock::now() >= deadline) co_return Result::Failure(std::move(timed_out));

  auto pending = std::make_shared<Pending>();
  auto start = [tasks, io = std::move(io), stop, deadline, timed_out, owner = pending, cancelled](
                   CallbackCompletion<Result> reply) mutable -> std::function<void()> {
    owner->operation = tasks.Launch([io = std::move(io), stop, deadline, timed_out, reply, cancelled]() mutable -> Task<void> {
      try {
        auto result = co_await std::move(io);
        if (stop.stop_requested()) reply.Complete(cancelled());
        else if (ModelClock::now() >= deadline) reply.Complete(Result::Failure(std::move(timed_out)));
        else reply.Complete(std::move(result));
      } catch (...) {
        reply.Fail(std::current_exception());
      }
    });
    owner->timer = tasks.Launch([deadline, error = std::move(timed_out), reply]() mutable -> Task<void> {
      auto time_left = deadline - ModelClock::now();
      if (time_left > ModelClock::duration::zero()) co_await Delay(time_left);
      reply.Complete(Result::Failure(std::move(error)));
    });
    owner->on_stop.emplace(stop, [tasks, reply, cancelled] {
      try { tasks.Post([reply, cancelled] { reply.Complete(cancelled()); }); }
      catch (const std::logic_error&) {}
    });
    return [owner] { owner->Cancel(); };
  };
  auto result = co_await AwaitCallback<Result>(std::move(start));
  pending->Cancel();
  if (stop.stop_requested()) co_return cancelled();
  co_return result;
}

using ModelBodyReader = std::function<Task<IoResult<Bytes>>(std::size_t)>;

std::unique_ptr<ModelTurnStream> MakeSseModelStream(
    TaskScope tasks, ModelBodyReader reader, std::stop_token stop, std::unique_ptr<SseModelProtocol> protocol,
    ModelStreamDeadlines deadlines);

} // namespace huxerui::agentkit::detail
