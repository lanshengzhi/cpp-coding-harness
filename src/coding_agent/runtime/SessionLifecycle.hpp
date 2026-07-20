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
struct AutomaticPublication {
    std::filesystem::path workspace;
    std::optional<std::filesystem::path> directory_override;
};

/// In-memory publication without any filesystem state.
struct InMemoryPublication {
    std::filesystem::path workspace;
};

/// Passive target intent for publishing one newly created Agent Session.
using NewSessionPublication = std::variant<
    ExplicitNewPublication,
    AutomaticPublication,
    InMemoryPublication>;

/// Read-only resume target preparation. Does not open a writable store and
/// does not modify the existing session file.
[[nodiscard]] util::Expected<PreparedResumeTarget> prepare_resume_target(
    std::filesystem::path resume_path,
    std::filesystem::path explicit_workspace,
    bool workspace_explicit);

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

} // namespace cch::coding_agent::runtime
