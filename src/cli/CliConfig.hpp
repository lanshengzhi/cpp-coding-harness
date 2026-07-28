#pragma once

#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>

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

/// The one passive CLI intent value: produced by argument parsing and consumed
/// by the CLI runtime directly after help handling. SessionFactory owns every
/// creation semantic for it (workspace, provider readiness, Agent Session
/// targets, Session Resume compatibility).
struct CliConfig {
    bool fake{false};
    bool print{false};
    bool enable_bash{false};
    std::optional<bool> project_trust_override;
    bool no_skills{false};
    bool no_prompt_templates{false};
    bool help{false};
    std::vector<std::string> prompt_template_paths;
    OutputMode output_mode{OutputMode::Text};
    bool workspace_explicit{false};
    /// Explicit turn cap forwarded to session creation; std::nullopt (the
    /// default) imposes no cap (ADR 0015). Set only by --max-turns.
    std::optional<int> max_turns{std::nullopt};
    /// Default workspace, resolved non-throwingly by parse_args from the
    /// current working directory; an unreadable cwd becomes a parse diagnostic.
    std::filesystem::path workspace;
    /// Normalized session intent: default persisted creation when no explicit
    /// target flag (--session/--resume/--no-session) was supplied.
    coding_agent::SessionTarget session_target{};
    /// Raw --session-dir value: the highest-priority automatic-directory
    /// override. Applies only to default persisted creation; explicit create
    /// and resume targets keep their exact paths.
    std::optional<std::string> session_dir;
    coding_agent::CliProviderOverrides provider_overrides;
    std::vector<std::string> file_arguments;
    std::string prompt;
    std::string help_text;
};

} // namespace cch::cli
