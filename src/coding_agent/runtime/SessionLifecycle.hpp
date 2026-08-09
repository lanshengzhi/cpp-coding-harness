#pragma once

#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/SessionStore.hpp"
#include "../../../include/cch/harness/session/SessionResume.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {

struct OpenSession {
    std::filesystem::path workspace;
    harness::session::SessionMetadata metadata;
    std::vector<ai::MessageVariant> history;
    std::unique_ptr<harness::session::SessionStore> store;
    /// Provider stored in session metadata (populated on resume).
    std::optional<std::string> stored_provider;
    /// Model stored in session metadata (populated on resume).
    std::optional<std::string> stored_model;
    /// Effective model found on the active resume path.
    std::optional<std::string> context_model;
    /// Effective thinking level found on the active resume path.
    std::optional<std::string> context_thinking_level;
    /// Shape of the restored session context.
    harness::session::SessionTopology topology{harness::session::SessionTopology::Linear};
};

struct PreparedResumeTarget {
    std::filesystem::path resume_path;
    std::filesystem::path workspace;
    harness::session::SessionResumeResult resume;
};

/// Explicit path publication for a newly created Agent Session.
struct ExplicitNewPublication {
    std::filesystem::path session_path;
    std::filesystem::path workspace;
};

/// Automatic publication under the Agent Config Directory, or directly inside
/// an already-resolved CLI directory override. Path derivation and identity
/// generation happen inside publish_session; the target carries intent only.
/// An optional `--session-id` names the new session (pi warn-create).
struct AutomaticPublication {
    std::filesystem::path workspace;
    std::optional<std::filesystem::path> directory_override;
    std::optional<std::string> session_id;
};

/// In-memory publication without any filesystem state. An optional
/// `--session-id` names the session (pi inMemory + `--session-id`).
struct InMemoryPublication {
    std::filesystem::path workspace;
    std::optional<std::string> session_id;
};

/// pi `appendSessionInfo` name sanitization (session-manager.ts): CR/LF runs
/// become one space, then the result is trimmed. Shared by the CLI `--name`
/// publication and the in-session `/name` flow.
[[nodiscard]] std::string sanitize_session_name(const std::string& name);

/// Passive target intent for publishing one newly created Agent Session.
using NewSessionPublication = std::variant<
    ExplicitNewPublication,
    AutomaticPublication,
    InMemoryPublication>;

/// Read-only resume target preparation. Does not open a writable store and
/// does not modify the existing session file. `cwd_override` (pi
/// `SessionManager.open(path, dir, cwdOverride)`) binds the runtime to the
/// override even when the header stores a different (missing) cwd; the
/// header keeps its stored value.
[[nodiscard]] util::Expected<PreparedResumeTarget> prepare_resume_target(
    std::filesystem::path resume_path,
    std::filesystem::path explicit_workspace,
    bool workspace_explicit,
    std::optional<std::filesystem::path> cwd_override = std::nullopt);

/// Publish a newly created session after all fallible prerequisites have
/// succeeded. Session Publication is the single mutation point: it derives
/// automatic targets, applies the directory privacy policy, generates the
/// session identity, and translates store failures into session errors.
[[nodiscard]] util::Expected<OpenSession> publish_session(
    NewSessionPublication target,
    std::string provider,
    std::string model);

/// Open an existing session file for resumption after all fallible
/// prerequisites have succeeded.
[[nodiscard]] util::Expected<OpenSession> publish_resume_session(
    const PreparedResumeTarget& target);

/// pi `SessionManager.forkFrom`: create a new persisted session in the target
/// directory (the effective session directory or the workspace-keyed default)
/// that inherits the source session's full history. The new file carries a
/// fresh header (new id, target cwd, `parentSession` = the resolved source
/// path); the returned prepared target re-resolves the copied history exactly
/// like a resume. Fails with pi's fork wording when the source is empty or
/// invalid or carries no header.
[[nodiscard]] util::Expected<PreparedResumeTarget> prepare_fork_target(
    std::filesystem::path source_path,
    std::filesystem::path target_workspace,
    std::optional<std::filesystem::path> directory_override,
    std::optional<std::string> session_id);

} // namespace cch::coding_agent::runtime
