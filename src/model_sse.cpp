#include "model_sse.h"

#include "runtime_internal.h"

namespace huxerui::agentkit::detail {

AgentResult<void> SseModelProtocol::End(std::deque<ModelEvent>&) {
  return Fail<void>(ErrorCode::ProtocolError, "Model stream ended before its terminal event");
}

AgentError ModelTimeoutError(ModelTimeout timeout) {
  auto error = Error(ErrorCode::DeadlineExceeded, "Model request exceeded its total time limit");
  switch (timeout) {
  case ModelTimeout::FirstResponse:
    error.message = "Timed out waiting for the first model response";
    error.provider_code = "first_response_timeout";
    break;
  case ModelTimeout::Idle:
    error.message = "Model stream stopped delivering events before completion";
    error.provider_code = "stream_idle_timeout";
    break;
  case ModelTimeout::Total:
    error.provider_code = "request_deadline";
    break;
  }
  return error;
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
  std::stop_source stop;
  std::unique_ptr<SseModelProtocol> protocol;
  ModelStreamDeadlines deadlines;
  SseReader wire;
  std::deque<ModelEvent> events;
  std::optional<AgentError> failure;
  std::optional<ModelClock::time_point> last_event;
  std::optional<ModelClock::time_point> terminal_deadline;
  bool reading = false;
  bool finished = false;
  std::unique_ptr<std::stop_callback<std::function<void()>>> stop_callback;

  void Stop() {
    if (!stop.request_stop()) return;
    reader.reset();
    events.clear();
  }

  void End() {
    auto ended = protocol->End(events);
    if (!ended.Succeeded()) failure = ended.Error();
    else finished = true;
    reader.reset();
  }
};

bool KeepAlive(std::string_view type) {
  return type == "ping" || type == "heartbeat" || type == "keep-alive";
}

Async<Bytes> ReadBody(std::shared_ptr<ModelBodyReader> reader, AgentError timeout) {
  auto bytes = co_await (*reader)(read_size);
  if (bytes.Succeeded()) co_return AgentResult<Bytes>::Success(std::move(bytes).Value());
  if (bytes.Error().code == IoErrorCode::Timeout) co_return AgentResult<Bytes>::Failure(std::move(timeout));
  auto error = Error(ErrorCode::ProviderFailure, "Model response body could not be read");
  error.provider_code = "stream_interrupted";
  co_return AgentResult<Bytes>::Failure(std::move(error));
}

Async<Bytes> NextBytes(const std::shared_ptr<StreamState>& state) {
  const auto next = state->last_event ? *state->last_event + state->deadlines.idle : state->deadlines.first;
  const auto kind = state->deadlines.total <= next ? ModelTimeout::Total :
      state->last_event ? ModelTimeout::Idle : ModelTimeout::FirstResponse;
  auto error = ModelTimeoutError(kind);
  auto until = std::min(next, state->deadlines.total);
  if (state->terminal_deadline) until = std::min(until, *state->terminal_deadline);
  auto read = ReadBody(state->reader, error);
  return WaitForModelIo(state->tasks, std::move(read), state->stop.get_token(), until, std::move(error));
}

Async<std::optional<ModelEvent>> ReadEvent(std::shared_ptr<StreamState> state) {
  using ReadResult = AgentResult<std::optional<ModelEvent>>;
  if (state->reading) co_return Fail<std::optional<ModelEvent>>(ErrorCode::Busy, "Another model read is open");
  state->reading = true;
  const auto reset = std::unique_ptr<bool, void(*)(bool*)>(&state->reading, [](bool* busy) { *busy = false; });
  if (state->stop.stop_requested()) co_return ReadResult::Success({});
  if (state->failure) co_return ReadResult::Failure(*state->failure);
  try {
    while (state->events.empty() && !state->finished && !state->stop.stop_requested()) {
      if (auto event = state->wire.Next()) {
        if (KeepAlive(event->type)) continue;
        auto decoded = state->protocol->Accept(std::move(*event), state->events);
        if (!decoded.Succeeded()) {
          state->failure = decoded.Error();
          break;
        }
        state->last_event = ModelClock::now();
        if (decoded.Value()) {
          state->finished = true;
          state->reader.reset();
        } else if (!state->terminal_deadline && state->protocol->HasTerminalFrame()) {
          state->terminal_deadline = *state->last_event + Duration{400};
        }
        continue;
      }
      auto bytes = co_await NextBytes(state);
      if (state->stop.stop_requested()) break;
      if (!bytes.Succeeded()) {
        if (state->terminal_deadline) state->End();
        else state->failure = bytes.Error();
        break;
      }
      if (bytes.Value().empty()) {
        if (!state->terminal_deadline && !state->wire.AtEventBoundary()) {
          state->failure = Error(ErrorCode::ProtocolError, "Model stream ended in an incomplete event");
          state->failure->provider_code = "stream_interrupted";
          break;
        }
        state->End();
        if (state->failure && !state->terminal_deadline) state->failure->provider_code = "stream_interrupted";
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
    TaskScope tasks, ModelBodyReader reader, std::stop_token stop, std::unique_ptr<SseModelProtocol> protocol,
    ModelStreamDeadlines deadlines) {
  auto state = std::make_shared<StreamState>();
  state->tasks = tasks;
  state->reader = std::make_shared<ModelBodyReader>(std::move(reader));
  state->deadlines = deadlines;
  state->protocol = std::move(protocol);
  state->stop_callback = std::make_unique<std::stop_callback<std::function<void()>>>(stop,
      [weak = std::weak_ptr<StreamState>(state), tasks] {
        try { tasks.Post([weak] { if (auto current = weak.lock()) current->Stop(); }); }
        catch (const std::logic_error&) {}
      });
  if (stop.stop_requested()) state->Stop();
  return std::make_unique<SseStream>(std::move(state));
}

} // namespace huxerui::agentkit::detail
