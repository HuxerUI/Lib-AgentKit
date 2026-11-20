#include "model_sse.h"

#include "runtime_internal.h"

namespace huxerui::agentkit::detail {

AgentResult<void> SseModelProtocol::End(std::deque<ModelEvent>&) {
  return Fail<void>(ErrorCode::ProtocolError, "Model stream ended before its terminal event");
}

namespace {

constexpr std::size_t read_size = 32 * 1024;
constexpr std::size_t maximum_frame = 4 * 1024 * 1024;

class SseReader {
public:
  bool AtEventBoundary() const { return offset_ == input_.size() && !has_data_ && frame_.type.empty(); }

  void Append(std::span<const std::byte> bytes) {
    input_.erase(0, offset_);
    offset_ = 0;
    input_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  std::optional<SseFrame> Next() {
    for (;;) {
      if (skip_lf_ && offset_ < input_.size()) {
        if (input_[offset_] == '\n') ++offset_;
        skip_lf_ = false;
      }
      auto line_end = input_.find_first_of("\r\n", offset_);
      if (line_end == std::string::npos) {
        if (input_.size() - offset_ > maximum_frame) throw std::length_error("SSE line exceeds limits");
        return std::nullopt;
      }
      auto line = std::string_view(input_).substr(offset_, line_end - offset_);
      skip_lf_ = input_[line_end] == '\r';
      offset_ = line_end + 1;
      if (std::exchange(first_line_, false) && line.starts_with("\xef\xbb\xbf")) line.remove_prefix(3);
      if (line.size() > maximum_frame) throw std::length_error("SSE line exceeds limits");
      if (line.empty()) {
        auto frame = std::exchange(frame_, {});
        frame_bytes_ = 0;
        if (!std::exchange(has_data_, false)) continue;
        frame.data.pop_back();
        if (frame.type.empty()) frame.type = "message";
        return frame;
      }
      if (!ValidUtf8(line)) throw std::invalid_argument("Invalid UTF-8 in event stream");
      const auto colon = line.find(':');
      const auto field = line.substr(0, colon);
      auto value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
      if (value.starts_with(' ')) value.remove_prefix(1);
      if (field == "event") frame_.type = value;
      if (field == "data") {
        auto new_size = value.size() + 1;
        if (new_size > maximum_frame - frame_bytes_) throw std::length_error("SSE event exceeds limits");
        frame_bytes_ += new_size;
        frame_.data.append(value);
        frame_.data.push_back('\n');
        has_data_ = true;
      }
    }
  }

private:
  std::string input_;
  std::size_t offset_ = 0;
  SseFrame frame_;
  std::size_t frame_bytes_ = 0;
  bool first_line_ = true;
  bool skip_lf_ = false;
  bool has_data_ = false;
};

struct StreamState {
  TaskScope tasks;
  std::shared_ptr<ModelBodyReader> reader;
  std::stop_token stop;
  std::unique_ptr<SseModelProtocol> protocol;
  SseReader wire;
  std::deque<ModelEvent> events;
  std::optional<AgentError> failure;
  std::optional<CallbackCompletion<IoResult<Bytes>>> pending;
  TaskHandle body_task;
  bool reading = false;
  bool stopped = false;
  bool finished = false;
  std::unique_ptr<std::stop_callback<std::function<void()>>> stop_callback;

  void Stop() {
    if (std::exchange(stopped, true)) return;
    body_task.Cancel();
    if (pending) {
      pending->Complete(IoResult<Bytes>::Success({}));
      pending.reset();
    }
    reader = {};
    events.clear();
  }
};

Task<IoResult<Bytes>> NextBytes(const std::shared_ptr<StreamState>& state) {
  return AwaitCallback<IoResult<Bytes>>([state](CallbackCompletion<IoResult<Bytes>> reply) {
    if (state->stopped || state->stop.stop_requested()) {
      reply.Complete(IoResult<Bytes>::Success({}));
      return std::function<void()>{};
    }
    state->pending = reply;
    auto reader = state->reader;
    state->body_task = state->tasks.Launch([reader = std::move(reader), reply]() -> Task<void> {
      try {
        auto result = co_await (*reader)(read_size);
        reply.Complete(std::move(result));
      } catch (...) {
        reply.Fail(std::current_exception());
      }
    });
    return std::function<void()>{[weak = std::weak_ptr<StreamState>(state)] {
      if (auto live = weak.lock()) live->Stop();
    }};
  });
}

Async<std::optional<ModelEvent>> ReadEvent(std::shared_ptr<StreamState> state) {
  using ReadResult = AgentResult<std::optional<ModelEvent>>;
  if (state->reading) co_return Fail<std::optional<ModelEvent>>(ErrorCode::Busy, "Another model read is open");
  state->reading = true;
  const auto reset = std::unique_ptr<bool, void(*)(bool*)>(&state->reading, [](bool* busy) { *busy = false; });
  if (state->stop.stop_requested()) state->Stop();
  if (state->stopped) co_return ReadResult::Success({});
  if (state->failure) co_return ReadResult::Failure(*state->failure);
  try {
    while (state->events.empty() && !state->finished && !state->stopped) {
      if (auto event = state->wire.Next()) {
        auto decoded = state->protocol->Accept(std::move(*event), state->events);
        if (!decoded.Succeeded()) {
          state->failure = decoded.Error();
          break;
        }
        state->finished = decoded.Value();
        if (state->finished) state->reader = {};
        continue;
      }
      auto bytes = co_await NextBytes(state);
      state->pending.reset();
      if (state->stop.stop_requested()) state->Stop();
      if (state->stopped) break;
      if (!bytes.Succeeded()) {
        state->failure = Error(ErrorCode::ProviderFailure, "Model response body could not be read");
        break;
      }
      if (bytes.Value().empty()) {
        if (!state->wire.AtEventBoundary()) {
          state->failure = Error(ErrorCode::ProtocolError, "Model stream ended in an incomplete event");
          break;
        }
        auto ended = state->protocol->End(state->events);
        if (!ended.Succeeded()) state->failure = ended.Error();
        else {
          state->finished = true;
          state->reader = {};
        }
        break;
      }
      state->wire.Append(bytes.Value());
    }
  } catch (const std::exception&) {
    state->failure = Error(ErrorCode::ProtocolError, "Invalid model event stream");
  }
  if (state->failure) {
    state->reader = {};
    state->events.clear();
    co_return ReadResult::Failure(*state->failure);
  }
  if (state->events.empty()) co_return ReadResult::Success({});
  auto event = std::move(state->events.front());
  state->events.pop_front();
  co_return ReadResult::Success(std::move(event));
}

class SseStream final : public ModelTurnStream {
public:
  explicit SseStream(std::shared_ptr<StreamState> state) : state_(std::move(state)) {}

  ~SseStream() override { state_->Stop(); }

  Async<std::optional<ModelEvent>> ReadAsync() override { return ReadEvent(state_); }

  Async<ModelStopOutcome> StopAsync() override {
    state_->Stop();
    co_return AgentResult<ModelStopOutcome>::Success(ModelStopOutcome::LocalStopped);
  }

private:
  std::shared_ptr<StreamState> state_;
};

} // namespace

std::unique_ptr<ModelTurnStream> MakeSseModelStream(
    TaskScope tasks, ModelBodyReader reader, std::stop_token stop, std::unique_ptr<SseModelProtocol> protocol) {
  auto state = std::make_shared<StreamState>();
  state->tasks = tasks;
  state->reader = std::make_shared<ModelBodyReader>(std::move(reader));
  state->stop = stop;
  state->protocol = std::move(protocol);
  state->stop_callback = std::make_unique<std::stop_callback<std::function<void()>>>(stop,
      [weak = std::weak_ptr<StreamState>(state), tasks] {
        try { tasks.Post([weak] { if (auto current = weak.lock()) current->Stop(); }); }
        catch (const std::logic_error&) {}
      });
  return std::make_unique<SseStream>(std::move(state));
}

} // namespace huxerui::agentkit::detail
