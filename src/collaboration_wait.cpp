#include "runtime_internal.h"

namespace huxerui::agentkit {
namespace detail {

bool RuntimeState::WaitCycle(RunId waiter, CollaborationMessageId request) const {
  return WaitCycle(std::move(waiter), RunWait{WaitKind::Reply, std::move(request), {}});
}

Async<ReplyOutcome> RuntimeState::WaitMessage(CollaborationMessageId id, WaitOptions wait, std::optional<RunId> actor) {
  using Result = AgentResult<ReplyOutcome>;
  auto initial = co_await ReadMessage(id, actor, Capability::WaitForResult);
  if (!initial.Succeeded()) co_return Result::Failure(initial.Error());
  const auto& record = initial.Value();
  if (!record.request || !std::holds_alternative<WorkRequest>(record.request->body)) {
    co_return Fail<ReplyOutcome>(ErrorCode::InvalidArgument, "Waiting requires a work request");
  }
  if (actor && record.summary.from.run != actor) {
    co_return Fail<ReplyOutcome>(ErrorCode::PermissionDenied, "Only the requesting run can wait for the reply");
  }
  auto deadline = wait.deadline;
  auto matches = [&](const RunWait& existing) {
    return existing.kind == WaitKind::Reply && std::get_if<CollaborationMessageId>(&existing.subject) &&
           std::get<CollaborationMessageId>(existing.subject) == id;
  };
  RunExecution* run = actor ? &runs.at(*actor) : nullptr;
  SessionData* session = run ? &sessions.at(run->view.session) : nullptr;
  if (run) {
    auto prior = std::ranges::find(run->view.waits, WaitKind::Reply, &RunWait::kind);
    if (prior != run->view.waits.end()) {
      if (!run->restoring || !matches(*prior)) co_return Fail<ReplyOutcome>(ErrorCode::Busy, "Run is already waiting for a reply");
      deadline = prior->deadline;
    }
    if (!record.outcome && (!deadline || Now() < *deadline)) {
      if (WaitCycle(*actor, id)) {
        if (!run->restoring) co_return Fail<ReplyOutcome>(ErrorCode::WaitCycle, "Reply wait would create a cycle");
      } else if (prior == run->view.waits.end()) {
        run->view.waits.push_back({WaitKind::Reply, id, deadline});
        UpdateRunState(*session, *run, RunState::WaitingForReply);
        ++session->sequence;
        auto saved = co_await WhenSaved(Persist(*session));
        if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
      }
    }
  }

  auto self = shared_from_this();
  using Event = AgentResult<std::optional<ReplyOutcome>>;
  auto latest = [&]() -> Async<std::optional<ReplyOutcome>> {
    auto updated = co_await ReadMessage(id, actor, Capability::WaitForResult);
    if (!updated.Succeeded()) co_return Event::Failure(updated.Error());
    if (updated.Value().outcome) co_return Event::Success(updated.Value().outcome);
    if (deadline && Now() >= *deadline) {
      co_return Event::Success(ReplyOutcome{id, ReplyEnd::WaitTimedOut, {}, updated.Value().summary.target_run});
    }
    if (actor && WaitCycle(*actor, id)) {
      co_return Event::Success(ReplyOutcome{id, ReplyEnd::WaitCycle, {}, updated.Value().summary.target_run});
    }
    auto next = deadline;
    const auto& request = *updated.Value().request;
    const auto& work = std::get<WorkRequest>(request.body);
    auto consider = [&](std::optional<TimePoint> point) {
      if (point && (!next || *point < *next)) next = point;
    };
    if (updated.Value().summary.delivery == DeliveryState::Queued) consider(request.deliver_by);
    if (work.reply) consider(work.reply->reply_by);
    auto lease = std::make_shared<std::optional<Subscription>>();
    TaskHandle observer;
    TaskHandle timer;
    auto subscribe = [self, id, actor, next, lease, &observer, &timer](CallbackCompletion<Event> complete) {
      observer = self->options.tasks.Launch([self, id, actor, complete, lease]() -> Task<void> {
        auto observed = co_await Observe<CollaborationRecord>(self,
            [weak = self->weak_from_this(), id, actor, complete](auto result) {
              if (!result.Succeeded()) { complete.Complete(Event::Failure(result.Error())); return; }
              auto state = weak.lock();
              if (!state) return;
              if (result.Value()->outcome) complete.Complete(Event::Success(result.Value()->outcome));
              else if (actor && state->WaitCycle(*actor, id)) {
                complete.Complete(Event::Success(ReplyOutcome{id, ReplyEnd::WaitCycle, {}, result.Value()->summary.target_run}));
              }
            }, [id, actor](RuntimeState& state) { return state.ReadMessage(id, actor, Capability::WaitForResult); });
        if (observed.Succeeded()) *lease = std::move(observed).Value();
        else complete.Complete(Event::Failure(observed.Error()));
      });
      if (next) {
        const auto duration = std::max(Duration::zero(), *next - self->Now());
        timer = self->options.tasks.Launch([complete, duration]() -> Task<void> {
          co_await Delay(duration);
          complete.Complete(Event::Success(std::nullopt));
        });
      }
      return [lease, observer, timer] {
        observer.Cancel();
        lease->reset();
        timer.Cancel();
      };
    };
    auto observed = co_await AwaitCallback<Event>(std::move(subscribe));
    observer.Cancel();
    timer.Cancel();
    lease->reset();
    co_return observed;
  };
  auto event = co_await latest();
  while (event.Succeeded() && !event.Value()) event = co_await latest();
  if (run && std::erase_if(run->view.waits, matches) != 0 && !run->stop.stop_requested()) {
    UpdateRunState(*session, *run, RunState::Running);
    ++session->sequence;
    auto saved = co_await WhenSaved(Persist(*session));
    if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
  }
  if (!event.Succeeded()) co_return Result::Failure(event.Error());
  co_return Result::Success(std::move(*event.Value()));
}

} // namespace detail

SessionCollaboration::SessionCollaboration(std::shared_ptr<detail::RuntimeState> runtime, std::optional<RunId> run)
    : runtime_(std::move(runtime)), run_(std::move(run)) {}

Async<Receipt<MessageAccepted>> SessionCollaboration::SendMessageAsync(CommandId command, SessionMessageRequest request) {
  return [](SessionCollaboration service, CommandId id, SessionMessageRequest value) -> Async<Receipt<MessageAccepted>> {
    co_return co_await service.runtime_->SendMessage(std::move(id), std::move(value), service.run_);
  }(*this, std::move(command), std::move(request));
}

Async<Receipt<MessageAccepted>> SessionCollaboration::ReplyAsync(CommandId command, CollaborationMessageId request, SessionReply reply) {
  return [](SessionCollaboration service, CommandId id, CollaborationMessageId target, SessionReply value) -> Async<Receipt<MessageAccepted>> {
    co_return co_await service.runtime_->ReplyMessage(std::move(id), std::move(target), std::move(value), service.run_);
  }(*this, std::move(command), std::move(request), std::move(reply));
}

Async<Receipt<MessageAccepted>> SessionCollaboration::StartRequestAsync(CommandId command, CollaborationMessageId request) {
  return [](SessionCollaboration service, CommandId id, CollaborationMessageId target) -> Async<Receipt<MessageAccepted>> {
    if (service.run_) co_return detail::Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "Only the receiving user can start queued work");
    co_return co_await service.runtime_->StartMessage(std::move(id), std::move(target));
  }(*this, std::move(command), std::move(request));
}

Async<CommandReceipt> SessionCollaboration::WithdrawMessageAsync(CommandId command, CollaborationMessageId message) {
  return [](SessionCollaboration service, CommandId id, CollaborationMessageId target) -> Async<CommandReceipt> {
    co_return co_await service.runtime_->WithdrawMessage(std::move(id), std::move(target), service.run_);
  }(*this, std::move(command), std::move(message));
}

Async<CollaborationRecord> SessionCollaboration::ReadMessageAsync(CollaborationMessageId message) {
  return [](SessionCollaboration service, CollaborationMessageId id) -> Async<CollaborationRecord> {
    co_return co_await service.runtime_->ReadMessage(std::move(id), service.run_, Capability::WaitForResult);
  }(*this, std::move(message));
}

Async<ReplyOutcome> SessionCollaboration::WaitForReplyAsync(CollaborationMessageId request, WaitOptions options) {
  return [](SessionCollaboration service, CollaborationMessageId id, WaitOptions config) -> Async<ReplyOutcome> {
    co_return co_await service.runtime_->WaitMessage(std::move(id), config, service.run_);
  }(*this, std::move(request), options);
}

SessionCollaboration AgentRuntime::Collaboration() { return SessionCollaboration(state_); }

SessionCollaboration RunContext::Collaboration() { return SessionCollaboration(runtime_, run_); }

SessionService RunContext::Sessions() { return SessionService(runtime_, run_); }

} // namespace huxerui::agentkit
