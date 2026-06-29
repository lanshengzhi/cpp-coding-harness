#pragma once

#include "../../../include/cch/coding_agent/Sdk.hpp"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct RpcModeConfig {
    std::istream& input;
    std::ostream& output;
    AgentSession& session;
    std::string provider;
    std::string model;
    std::filesystem::path workspace;
};

[[nodiscard]] int run_rpc_mode(RpcModeConfig config);

} // namespace cch::coding_agent::runtime
