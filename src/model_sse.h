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
};

using ModelBodyReader = std::function<Task<IoResult<Bytes>>(std::size_t)>;

std::unique_ptr<ModelTurnStream> MakeSseModelStream(
    TaskScope tasks, ModelBodyReader reader, std::stop_token stop, std::unique_ptr<SseModelProtocol> protocol);

} // namespace huxerui::agentkit::detail
