#pragma once

#include <filesystem>
#include <string>

namespace cch::cli {

struct AsyncCliRuntimeConfig {
    bool fake{false};
    bool repl{false};
    bool enable_bash{false};
    int max_turns{8};
    std::filesystem::path workspace;
    std::string model;
    std::string base_url;
    std::string api_key_env;
    std::string prompt;
};

int run_async_cli(const AsyncCliRuntimeConfig& config);

} // namespace cch::cli
