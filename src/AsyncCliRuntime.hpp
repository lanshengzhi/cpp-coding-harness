#pragma once

#include <filesystem>
#include <string>

namespace cch::cli {

enum class OutputMode {
    Text,
    Json,
};

struct AsyncCliRuntimeConfig {
    bool fake{false};
    bool repl{false};
    bool enable_bash{false};
    OutputMode output_mode{OutputMode::Text};
    int max_turns{8};
    bool workspace_explicit{false};
    std::filesystem::path workspace;
    std::filesystem::path session_path;
    std::filesystem::path resume_path;
    std::string session_id;
    std::string created_at;
    std::string model;
    std::string base_url;
    std::string api_key_env;
    std::string prompt;
};

int run_async_cli(const AsyncCliRuntimeConfig& config);

} // namespace cch::cli
