#pragma once

#include "AgentSessionCreationRequest.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::coding_agent::runtime {

/// pi fork `position`: `"before"` forks before the chosen user message (the
/// editor pre-fills its text); `"at"` branches at the entry itself.
enum class ForkPosition { Before, At };

/// One fork-selectable user message (pi `getUserMessagesForForking`).
struct UserForkMessage {
    /// The session entry id: the store's real entry ids for both
    /// alternatives (pi in-memory SessionManagers assign entry ids too).
    std::string entry_id;
    std::string text;
};

/// Outcome of preparing one in-session fork (pi `AgentSessionRuntime.fork`).
struct ForkPreparation {
    /// The written branched session file (persisted sources). A fork whose
    /// target leaf is absent (a root user message) yields a fresh empty
    /// session file with the `parentSession` pointer, exactly like pi's
    /// `SessionManager.create + newSession({parentSession})` path.
    std::optional<std::filesystem::path> branched_path;
    /// pi `selectedText`: the chosen user message's text for position
    /// "before"; nullopt for position "at".
    std::optional<std::string> selected_text;
    /// The in-memory branch seed (in-memory sources only); the empty-context
    /// seed covers the no-target-leaf in-memory case (pi `newSession` with
    /// the parent pointer).
    std::optional<InMemoryBranchSeed> in_memory_seed;
};

/// Persisted fork source facts: the authoritative session file; the branch
/// writes into its parent directory (pi `getSessionDir()`).
struct PersistedForkSource {
    std::filesystem::path session_path;
};

/// In-memory fork source facts: the session's store; its live tree is the
/// entry surface (pi in-memory SessionManager entries; the branch travels
/// as a seed).
struct InMemoryForkSource {
    std::shared_ptr<harness::session::SessionStore> store;
};

/// The closed set of fork-source alternatives (CODING_STANDARDS.md §4.2).
using ForkSourceVariant = std::variant<PersistedForkSource, InMemoryForkSource>;

/// Source facts for one in-session fork (the current Agent Session).
struct ForkSource {
    /// The workspace the source session binds to (the branch header cwd).
    std::filesystem::path workspace;
    /// The source alternative; a default-constructed (empty-path) persisted
    /// alternative is the absent-source state and fails like an invalid
    /// entry.
    ForkSourceVariant source{PersistedForkSource{}};
};

/// pi `getUserMessagesForForking`: every user message with non-blank text in
/// session order, from the file's entries (persisted) or the store's live
/// tree (in-memory). Tool-result-echoed and blank messages are skipped.
[[nodiscard]] std::vector<UserForkMessage> user_messages_for_forking(
    const ForkSource& source);

/// pi `AgentSessionRuntime.fork` preparation: validates the entry, computes
/// the branch target (position "before" requires a user message and branches
/// before it; position "at" branches at the entry itself), and either writes
/// the branched session file or produces the in-memory seed.
///
/// Verbatim pi errors: "Invalid entry ID for forking" (unknown entry, or a
/// non-user entry with position "before"), "This session has not been saved
/// yet. Wait for the first assistant response before cloning or forking it."
/// (persisted source whose file is missing), and "Failed to create forked
/// session" (branch file load/write failures).
[[nodiscard]] support::Expected<ForkPreparation> prepare_fork(
    const ForkSource& source,
    std::string_view entry_id,
    ForkPosition position);

} // namespace cch::coding_agent::runtime
