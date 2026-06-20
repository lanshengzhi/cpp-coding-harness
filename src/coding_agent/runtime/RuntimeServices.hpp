#pragma once

#include "../../../include/cch/agent/ToolRegistry.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/coding_agent/PromptTemplateLoader.hpp"
#include "../../../include/cch/coding_agent/SkillLoader.hpp"
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
    /// Skill directories to scan at startup. Empty by default (no skill loading).
    std::vector<SkillDirSpec> skill_dirs;
    /// Prompt template directories or file paths to scan at startup.
    std::vector<std::string> prompt_dirs;
    /// Print skill loading diagnostics to stderr. Default true.
    bool print_skill_diagnostics{true};
};

struct RuntimeServices {
    std::unique_ptr<ai::StreamingChatClient> client;
    std::shared_ptr<harness::AsyncExecutionEnv> env;
    agent::AsyncToolRegistry tools;
    /// Result of skill loading at startup (empty if skill_dirs was empty).
    SkillLoadResult skill_load_result;
    /// Result of prompt template loading at startup (empty if prompt_dirs was empty).
    PromptTemplateLoadResult prompt_load_result;
};

[[nodiscard]] util::Expected<RuntimeServices> make_runtime_services(const RuntimeServicesConfig& config);

} // namespace cch::coding_agent::runtime
