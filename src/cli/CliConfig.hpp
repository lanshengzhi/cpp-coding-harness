#pragma once

#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
    bool print{false};
    std::optional<bool> project_trust_override;
    bool no_skills{false};
    bool no_prompt_templates{false};
    bool no_context_files{false};
    bool no_themes{false};
    bool help{false};
    bool version{false};
    std::vector<std::string> prompt_template_paths;
    /// Repeatable pi `--skill` paths: explicit skills load even when
    /// `--no-skills` drops discovery.
    std::vector<std::string> skills;
    /// Repeatable pi `--theme` paths (file or directory). `--no-themes` skips
    /// auto-discovery only; explicit paths stay effective.
    std::vector<std::string> themes;
    OutputMode output_mode{OutputMode::Text};
    /// The internal workspace containment seam: always the current working
    /// directory (pi `workspace := cwd`), resolved non-throwingly by
    /// parse_args; an unreadable cwd becomes a parse diagnostic.
    std::filesystem::path workspace;
    /// Normalized session intent: default persisted creation when no explicit
    /// target flag (--session/--resume/--no-session) was supplied. The
    /// remaining session-family flags (--continue/--session-id/--fork/--name)
    /// carry their raw values below; session-family semantics assemble them
    /// (pi session-family CLI).
    coding_agent::SessionTarget session_target{};
    /// Raw --session-dir value: the highest-priority automatic-directory
    /// override. Applies only to default persisted creation; explicit create
    /// and resume targets keep their exact paths.
    std::optional<std::string> session_dir;
    /// pi CLI model selection: `--provider` (default provider name),
    /// `--model` (model pattern), `--models` (comma-separated cycling
    /// patterns), and `--api-key` (in-memory runtime API key override, never
    /// persisted). `--api-key` requires an explicit model.
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::vector<std::string> models;
    std::optional<std::string> api_key;
    /// pi `--thinking <level>` (off, minimal, low, medium, high, xhigh, max).
    std::optional<std::string> thinking;
    /// pi session-family raw values: `--continue`, `--session-id`, `--fork`,
    /// and `--name` carry pi's spellings and shorts; the session-family
    /// semantics own their normalization.
    bool continue_session{false};
    std::optional<std::string> session_id;
    std::optional<std::string> fork;
    std::optional<std::string> name;
    /// pi `--list-models [search]`: has_value() when requested; an empty
    /// string is the bare flag, a non-empty string the fuzzy search pattern.
    std::optional<std::string> list_models;
    /// pi `--system-prompt` text-or-file; `--append-system-prompt` is
    /// repeatable (appended with pi's "\n\n" joining).
    std::optional<std::string> system_prompt;
    std::vector<std::string> append_system_prompt;
    std::vector<std::string> file_arguments;
    std::string prompt;
    std::string help_text;
};

/// The C++ binary's own version (CMake project version), printed by
/// `--version`.
[[nodiscard]] std::string_view project_version();

} // namespace cch::cli
