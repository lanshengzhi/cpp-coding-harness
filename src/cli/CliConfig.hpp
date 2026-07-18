#pragma once

#include "CliRuntimeConfig.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::cli {

struct CliConfig {
    bool fake{false};
    bool repl{false};
    bool enable_bash{false};
    std::optional<bool> project_trust_override;
    bool no_skills{false};
    bool no_prompt_templates{false};
    bool help{false};
    std::vector<std::string> prompt_template_paths;
    OutputMode output_mode{OutputMode::Text};
    bool workspace_explicit{false};
    int max_turns{8};
    std::filesystem::path workspace{std::filesystem::current_path()};
    /// Normalized session intent: default persisted creation when neither
    /// --session nor --resume was supplied.
    coding_agent::SessionTarget session_target{};
    coding_agent::CliProviderOverrides provider_overrides;
    std::string prompt;
    std::string help_text;
};

} // namespace cch::cli
