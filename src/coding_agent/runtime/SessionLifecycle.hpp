#pragma once

#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/SessionStore.hpp"
#include "../../../include/cch/harness/session/SessionResume.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "../SessionPathPolicy.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct SessionOpenRequest {
    std::filesystem::path session_path;
    std::filesystem::path resume_path;
    std::filesystem::path workspace;
    bool workspace_explicit{false};
    std::string session_id;
    std::string created_at;
    std::string provider;
    std::string model;
};

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

/// Read-only resume target preparation. Does not open a writable store and
/// does not modify the existing session file.
[[nodiscard]] util::Expected<PreparedResumeTarget> prepare_resume_target(
    std::filesystem::path resume_path,
    std::filesystem::path explicit_workspace,
    bool workspace_explicit);

/// Publish a new session file after all fallible prerequisites have succeeded.
[[nodiscard]] util::Expected<OpenSession> publish_new_session(
    std::filesystem::path session_path,
    std::filesystem::path workspace,
    harness::session::SessionMetadata metadata);

/// Publish an automatically named session under the Agent Config Directory,
/// or directly inside a CLI-supplied custom directory. This is the only
/// publication path that creates or tightens harness-owned default
/// directories; a custom directory is created privately when missing but an
/// existing one's mode is never changed. All path derivation remains
/// side-effect free.
[[nodiscard]] util::Expected<OpenSession> publish_automatic_session(
    const session_paths::AutomaticSessionTarget& target,
    std::string provider,
    std::string model);

/// Publish an in-memory session without creating any filesystem state.
[[nodiscard]] OpenSession publish_in_memory_session(
    std::filesystem::path workspace,
    harness::session::SessionMetadata metadata);

/// Open an existing session file for resumption after all fallible
/// prerequisites have succeeded.
[[nodiscard]] util::Expected<OpenSession> publish_resume_session(
    const PreparedResumeTarget& target);

/// Legacy one-step open for tests. Prefer prepare/publish for production paths.
[[nodiscard]] util::Expected<OpenSession> open_session(SessionOpenRequest request);

} // namespace cch::coding_agent::runtime
