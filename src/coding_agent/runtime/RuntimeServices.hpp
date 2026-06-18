#pragma once

#include "../../../include/cch/agent/ToolRegistry.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/harness/LocalExecutionEnv.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace cch::coding_agent::runtime {

struct RuntimeServicesConfig {
    std::filesystem::path workspace;
    bool enable_bash{false};
    std::string provider_name;
    std::string model;
    std::string base_url;
    std::string api_key_env;
};

struct RuntimeServices {
    std::unique_ptr<ai::StreamingChatClient> client;
    std::shared_ptr<harness::AsyncExecutionEnv> env;
    agent::AsyncToolRegistry tools;
};

[[nodiscard]] util::Expected<RuntimeServices> make_runtime_services(const RuntimeServicesConfig& config);

} // namespace cch::coding_agent::runtime
