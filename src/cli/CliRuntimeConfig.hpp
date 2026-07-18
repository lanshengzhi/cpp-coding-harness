#pragma once

#include "../../include/cch/coding_agent/Sdk.hpp"
#include "../../include/cch/coding_agent/Settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::cli {

enum class OutputMode {
    Text,
    Json,
    Rpc,
};

struct AsyncCliRuntimeConfig {
    bool fake{false};
    bool repl{false};
    bool enable_bash{false};
    std::optional<bool> project_trust_override;
    bool disable_project_skills{false};
    bool disable_prompt_templates{false};
    std::vector<std::string> prompt_template_paths;
    OutputMode output_mode{OutputMode::Text};
    int max_turns{8};
    bool workspace_explicit{false};
    std::filesystem::path workspace;
    /// Normalized session intent. An omitted --session/--resume selects
    /// workspace-keyed default persistence, never an empty-path sentinel.
    coding_agent::SessionTarget session_target{};
    coding_agent::CliProviderOverrides provider_overrides;
    std::string prompt;
};

} // namespace cch::cli
