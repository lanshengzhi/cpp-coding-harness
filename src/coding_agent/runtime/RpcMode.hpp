#pragma once

#include "AgentSessionRuntime.hpp"

#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct RpcModeConfig {
    std::istream& input;
    std::ostream& output;
    AgentSessionRuntime& runtime;
    std::string provider;
    std::string model;
    std::filesystem::path workspace;
};

[[nodiscard]] int run_rpc_mode(RpcModeConfig config);

} // namespace cch::coding_agent::runtime
