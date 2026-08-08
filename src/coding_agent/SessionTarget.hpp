#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace cch::coding_agent {

/// Create a persisted Agent Session beneath the workspace-keyed Agent Config
/// Directory sessions root, or directly inside a CLI automatic-directory
/// override (pi `SessionManager.create`). An optional `--session-id` names
/// the new session (pi warn-create flow).
struct DefaultPersistedSessionTarget {
    std::optional<std::string> session_id;
};

/// pi `SessionManager.open`: open the session at an exact path, creating a
/// new session there when the file does not exist (pi `--session <path>`).
/// A global ID resolution for the same flag forks instead (cross-project
/// fork prompt) and never reaches this target.
struct ExplicitOpenOrCreateSessionTarget {
    std::filesystem::path path;
};

/// Resume the persisted Agent Session at an exact caller-supplied path.
struct ExplicitResumeSessionTarget {
    std::filesystem::path path;
};

/// pi `SessionManager.forkFrom`: create a new persisted session that inherits
/// the source session's full history. The CLI already rejected a local
/// session carrying the target id (`--fork --session-id` conflict check), so
/// the optional id is free for the new session.
struct ForkSessionTarget {
    std::filesystem::path source_path;
    std::optional<std::string> session_id;
};

/// pi `SessionManager.continueRecent`: resume the most recently modified
/// session in the effective session directory (cwd-filtered only when a
/// custom automatic-directory override is in effect), or create a new
/// persisted session when none exists.
struct ContinueRecentSessionTarget {};

/// Create an Agent Session without a session directory or transcript file.
/// An optional `--session-id` names the in-memory session (pi inMemory +
/// `--session-id`, allowed alongside `--no-session`).
struct InMemorySessionTarget {
    std::optional<std::string> session_id;
};

/// Session intent assembled by the CLI runtime from the pi session-family
/// flags (pi main.ts `createSessionManager`). Default construction selects
/// workspace-keyed persistence under the Agent Config Directory.
using SessionTarget = std::variant<
    DefaultPersistedSessionTarget,
    ExplicitOpenOrCreateSessionTarget,
    ExplicitResumeSessionTarget,
    ForkSessionTarget,
    ContinueRecentSessionTarget,
    InMemorySessionTarget>;

} // namespace cch::coding_agent
