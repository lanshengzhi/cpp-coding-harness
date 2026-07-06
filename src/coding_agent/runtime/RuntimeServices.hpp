#pragma once

#include "../../../include/cch/agent/ToolRegistry.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/coding_agent/PromptTemplate.hpp"
#include "../../../include/cch/coding_agent/Skill.hpp"
#include "../../../include/cch/harness/LocalExecutionEnv.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace cch::coding_agent::runtime {

struct RuntimeServicesConfig {
    std::filesystem::path workspace;
    bool enable_bash{false};
    std::string provider_registry_name;
    std::string provider;
    std::string api;
    std::string model;
    std::string base_url;
    std::string api_key;
    std::string api_key_env;
    std::vector<Skill> skills;
    std::vector<PromptTemplate> prompt_templates;
};

struct RuntimeServices {
    std::unique_ptr<ai::StreamingChatClient> client;
    std::shared_ptr<harness::AsyncExecutionEnv> env;
    agent::AsyncToolRegistry tools;
    std::vector<Skill> skills;
    std::vector<PromptTemplate> prompt_templates;
};

[[nodiscard]] util::Expected<RuntimeServices> make_runtime_services(RuntimeServicesConfig config);

} // namespace cch::coding_agent::runtime
