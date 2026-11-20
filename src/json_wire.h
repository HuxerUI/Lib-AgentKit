#pragma once

#include <huxerui/agentkit/types.h>
#include <nlohmann/json_fwd.hpp>

namespace huxerui::agentkit::detail {

nlohmann::json ToWireJson(const JsonValue& value);

JsonValue FromWireJson(const nlohmann::json& value);

} // namespace huxerui::agentkit::detail
