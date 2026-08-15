#pragma once

#include <cch/support/Error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::session_discovery {

/// Minimal session header facts for CLI session-family resolution (pi
/// `SessionInfo` subset): the file path, the header session id, the header
/// cwd, and the file modification time.
struct SessionInfoLite {
    std::filesystem::path path;
    std::string id;
    std::string cwd;
    std::filesystem::file_time_type modified{};
};

/// pi `SessionInfo` — the full selector-facing session facts
/// (session-manager.ts `buildSessionInfo`): the file path, header id/cwd,
/// the latest non-blank `session_info` name, the header parent session, the
/// header creation timestamp, the modification time (latest message
/// activity, else the header timestamp, else the file mtime), the message
/// count, the first user message text, and the space-joined user/assistant
/// message texts. `first_message` defaults to "(no messages)" like pi.
struct SessionInfo {
    std::filesystem::path path;
    std::string id;
    std::string cwd;
    std::optional<std::string> name;
    std::optional<std::filesystem::path> parent_session_path;
    std::string created;
    std::filesystem::file_time_type modified{};
    std::size_t message_count{0};
    std::string first_message;
    std::string all_messages_text;
};

/// pi `buildSessionInfo`: parse one session file into selector-facing facts.
/// Returns nullopt for unreadable files and for files whose first entry is
/// not a session header. Malformed later lines are skipped (pi
/// `parseSessionEntryLine` returns null and the loop continues).
[[nodiscard]] std::optional<SessionInfo> build_session_info(
    const std::filesystem::path& path);

/// pi `SessionManager.list`: every `*.jsonl` file in `directory` whose first
/// line parses as a session header, newest modification time first. When
/// `cwd_filter` is set, only sessions whose header cwd resolves to the filter
/// path are kept (an empty header cwd never matches). Best effort: a missing
/// directory, unreadable files, and header parse failures yield an empty
/// list (pi `listSessionsFromDir` catches and returns empty).
[[nodiscard]] std::vector<SessionInfo> list_sessions_info(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter);

/// pi `SessionManager.listAll`: with a custom session directory, the sessions
/// in that directory; otherwise every session under the sessions root's
/// per-project directories (one level deep, no cwd filter). Best effort,
/// newest first. `listAll` powers the in-session selector's "All" scope.
[[nodiscard]] std::vector<SessionInfo> list_all_sessions_info(
    const std::filesystem::path& sessions_root,
    const std::optional<std::filesystem::path>& custom_directory);

/// pi `SessionManager.list`: every `*.jsonl` file in `directory` whose first
/// line parses as a session header, newest modification time first. When
/// `cwd_filter` is set, only sessions whose header cwd resolves to the filter
/// path are kept (an empty header cwd never matches). Best effort: a missing
/// directory, unreadable files, and header parse failures yield an empty
/// list (pi `listSessionsFromDir` catches and returns empty).
[[nodiscard]] std::vector<SessionInfoLite> list_sessions_in_directory(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter);

/// pi `SessionManager.listAll`: with a custom session directory, the sessions
/// in that directory; otherwise every session under the sessions root's
/// per-project directories (one level deep, no cwd filter). Best effort,
/// newest first.
[[nodiscard]] std::vector<SessionInfoLite> list_all_sessions(
    const std::filesystem::path& sessions_root,
    const std::optional<std::filesystem::path>& custom_directory);

/// pi `findMostRecentSession`: the newest session in `directory`, optionally
/// cwd-filtered; nullopt when none (or the directory is unreadable).
[[nodiscard]] std::optional<SessionInfoLite> find_most_recent_session(
    const std::filesystem::path& directory,
    const std::optional<std::filesystem::path>& cwd_filter);

/// pi `assertValidSessionId`: the id must be non-empty, contain only
/// alphanumeric characters, '-', '_', and '.', and start and end with an
/// alphanumeric character. Returns pi's exact rejection message, or nullopt
/// when the id is valid.
[[nodiscard]] std::optional<std::string> invalid_session_id_reason(
    const std::string& id);

/// pi `resolveSessionPath` classification for one `--session`/`--fork`
/// argument: a path-like argument (contains '/' or '\' or ends with
/// ".jsonl") resolves to an exact path; otherwise the argument is matched as
/// a session id, exact then prefix, local first then global.
enum class SessionArgKind { Path, Local, Global, NotFound };

struct ResolvedSessionArg {
    SessionArgKind kind{SessionArgKind::NotFound};
    /// The resolved session file path (Path/Local/Global matches).
    std::filesystem::path path;
    /// The owning project cwd for global matches (pi's cross-project fork
    /// prompt shows it).
    std::string cwd;
    /// The original argument (pi's not-found message echoes it).
    std::string arg;
};

/// Resolve one session-family argument (pi `resolveSessionPath`). Path-like
/// values resolve against `workspace` with tilde expansion; id values match
/// against the local session directory (with the engaged cwd filter) and
/// then the global session space. A custom directory narrows the global
/// search to that directory (pi `listAll`).
[[nodiscard]] ResolvedSessionArg resolve_session_arg(
    const std::string& arg,
    const std::filesystem::path& workspace,
    const std::filesystem::path& local_directory,
    const std::optional<std::filesystem::path>& cwd_filter,
    const std::filesystem::path& sessions_root,
    const std::optional<std::filesystem::path>& custom_directory);

} // namespace cch::coding_agent::session_discovery
