#pragma once

#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include <cch/coding_agent/Settings.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::cli {

/// The one passive CLI intent value: produced by argument parsing and consumed
/// by the CLI runtime directly after help handling. SessionFactory owns every
/// creation semantic for it (workspace, provider readiness, Agent Session
/// targets, Session Resume compatibility).
struct CliConfig {
    bool print{false};
    /// The CLI-owned session facts value: Project Trust override, resource
    /// flags, Skill/Prompt Template/Theme paths, system-prompt values, model
    /// selection, and in-memory runtime API key.
    coding_agent::runtime::InteractiveSessionFacts session_facts{};
    bool help{false};
    bool version{false};
    /// The internal workspace containment seam: always the current working
    /// directory (pi `workspace := cwd`), resolved non-throwingly by
    /// parse_args; an unreadable cwd becomes a parse diagnostic.
    std::filesystem::path workspace;
    /// pi session-family raw flags (pi args.ts surface). The CLI runtime
    /// assembles the session target from these in pi's boot order (pi main.ts
    /// `createSessionManager`): `--session` opens-or-creates at a path or
    /// resolves an id (cross-project fork prompt), `--resume` opens the
    /// startup-TUI session picker (pi boolean flag — a following token is a
    /// positional message, never a path), `--continue` resumes the most
    /// recent session, `--fork` forks from a target id,
    /// `--session-id` names/validates/conflicts/warns-creates, `--name` sets
    /// the session display name, and `--no-session` short-circuits silently.
    /// The `--session`/`--fork` values engage only for non-empty values: pi's
    /// hand parser treats an empty value as absent (args.ts truthiness).
    std::optional<std::string> session_value;
    /// pi `--resume, -r` boolean: the startup-TUI session picker.
    bool resume{false};
    bool no_session_flag{false};
    bool continue_session{false};
    std::optional<std::string> session_id;
    std::optional<std::string> fork;
    std::optional<std::string> name;
    /// Raw --session-dir value: the highest-priority automatic-directory
    /// override (pi: --session-dir, then PI_CODING_AGENT_SESSION_DIR, then
    /// settings sessionDir). Consulted for default persisted creation and for
    /// session listing during session-family resolution.
    std::optional<std::string> session_dir;
    /// pi `--thinking <level>` (off, minimal, low, medium, high, xhigh, max).
    std::optional<std::string> thinking;
    /// pi `--list-models [search]`: has_value() when requested; an empty
    /// string is the bare flag, a non-empty string the fuzzy search pattern.
    std::optional<std::string> list_models;
    std::vector<std::string> file_arguments;
    /// Positional messages in CLI order (pi `parsed.messages`): the first
    /// merges into the initial prompt, the rest prompt sequentially.
    std::vector<std::string> messages;
    std::string help_text;
};

/// The C++ binary's own version (CMake project version), printed by
/// `--version`.
[[nodiscard]] std::string_view project_version();

} // namespace cch::cli
