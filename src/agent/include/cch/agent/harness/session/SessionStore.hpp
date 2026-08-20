#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::harness::session {

class InMemorySessionStore;
class JsonlSessionStore;

/// The Runtime-facing Session Store: a closed concrete facade over the two
/// supported persistence alternatives — a JSONL session file and an
/// in-memory no-persistence store (ADR 0040) — and the authoritative owner
/// of the session's live tree state. The seam is closed to extension and
/// exposes no generic serialization machinery; encoding and journal I/O stay
/// private behind the typed append operations.
///
/// Every store caches a live SessionTree built exactly once at
/// create/open time; a successful append both persists (JSONL sessions) and
/// incrementally updates the cached tree, so topology queries and context
/// reconstruction never re-read or re-parse the session file (pi
/// SessionManager, which keeps `fileEntries`/`byId` in memory and advances
/// them in `_appendEntry`). In-memory sessions update the same live tree
/// without disk I/O.
///
/// Mutating appends and tree queries are internally synchronized so the
/// serialized Session Event Commitment channel can execute appends on
/// Runtime worker threads while rare session-assembly appends (model change,
/// compaction, …) and tree queries run on the Session's loop. Every query
/// returns owned snapshot values — no borrowed view of the live tree escapes
/// the lock, so a concurrent append can never invalidate a result. Entry
/// order across the two append paths is the caller's responsibility (the
/// commitment channel drains before a mid-run typed append).
class SessionStore final {
public:
    /// Create a new JSONL session file (header written durably) with an
    /// empty live tree (pi `SessionManager.create`).
    [[nodiscard]] static support::Expected<SessionStore> create_new(
        const std::filesystem::path& path,
        SessionMetadata metadata);
    /// Open an existing JSONL session file; the persisted entries are parsed
    /// exactly once here to build the live tree (pi `SessionManager.open`).
    [[nodiscard]] static support::Expected<SessionStore> open_existing(
        const std::filesystem::path& path);
    /// The in-memory alternative: appends update the live tree without disk
    /// I/O and path() is empty (in-memory sessions are not resumable, #409).
    [[nodiscard]] static SessionStore in_memory(SessionMetadata metadata = {});
    /// One-shot read of a session file that is NOT the live session (the
    /// fork flows parse foreign files). Never use it to query the live
    /// session — the cached tree already answers those queries.
    [[nodiscard]] static support::Expected<LoadedSession> load(
        const std::filesystem::path& path);

    SessionStore(SessionStore&&) noexcept;
    SessionStore& operator=(SessionStore&&) noexcept;
    ~SessionStore();
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    /// Append one completed Session Entry message (pi `appendEntry`).
    [[nodiscard]] support::ExpectedVoid append(const ai::MessageVariant& message);

    // --- v3 tree entry appends; they persist (JSONL sessions) and advance
    // the live tree in one locked step ---

    [[nodiscard]] support::ExpectedVoid append_model_change(
        std::optional<std::string> parent_id,
        std::string provider,
        std::string model_id);
    [[nodiscard]] support::ExpectedVoid append_thinking_level_change(
        std::optional<std::string> parent_id,
        std::string thinking_level);
    [[nodiscard]] support::ExpectedVoid append_label_change(
        std::optional<std::string> parent_id,
        std::string target_id,
        std::optional<std::string> label);
    /// Append one Compaction entry (pi `appendCompaction`); the entry value
    /// carries pi's full field set (`summary`, `firstKeptEntryId`,
    /// `tokensBefore`, `retainedTail`, `details`, `usage`, `fromHook`).
    [[nodiscard]] support::ExpectedVoid append_compaction(
        std::optional<std::string> parent_id,
        CompactionEntryValue value);
    /// pi `appendBranchSummary`: record the summary of an abandoned branch
    /// (`from_id` is the abandoned leaf).
    [[nodiscard]] support::ExpectedVoid append_branch_summary(
        std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook);
    [[nodiscard]] support::ExpectedVoid append_session_info(
        std::optional<std::string> parent_id,
        std::string name);
    /// Write a Leaf entry to persist the current active leaf position
    /// (`target_id` nullopt writes the root leaf, pi `setLeafId(null)`) and
    /// move the live tree's leaf with it.
    [[nodiscard]] support::ExpectedVoid append_leaf(
        std::optional<std::string> parent_id,
        std::optional<std::string> target_id);

    // --- Live tree queries (snapshots taken under the append lock) ---

    /// pi `getTree`: root nodes with children sorted by timestamp (oldest
    /// first, newest at bottom) and resolved labels/label timestamps.
    [[nodiscard]] std::vector<SessionTreeNode> tree() const;
    /// One self-consistent topology snapshot: the root nodes and the active
    /// leaf id under the same append lock, so a concurrent append can never
    /// yield a roots/leaf pair from two different points in time.
    [[nodiscard]] SessionTreeSnapshot tree_snapshot() const;
    /// pi `buildSessionContext`: the compaction-aware LLM context of the
    /// active leaf path.
    [[nodiscard]] SessionContext build_context() const;
    /// pi `branch`: move the live tree's active leaf to an existing entry.
    /// The move is live-tree state only — persist the position with
    /// append_leaf (the C++ leaf-marker durability discipline).
    [[nodiscard]] support::ExpectedVoid branch(std::string_view target_id);
    /// pi `resetLeaf`: move the live tree's active leaf to the root
    /// position (before any entries).
    void reset_leaf();
    /// pi `getLeafId`: the active leaf entry id; empty at the root position.
    [[nodiscard]] std::string leaf_id() const;
    /// pi `getEntry`: a snapshot copy of one entry, nullopt when absent.
    [[nodiscard]] std::optional<SessionEntry> get_entry(
        std::string_view entry_id) const;
    /// The effective parent id for leaf-to-root traversal: the explicit
    /// wire parent, or the inferred linear-chain parent when the entry
    /// carries none (the SessionTree reading shared by `get_branch`).
    [[nodiscard]] std::optional<std::string> effective_parent_id(
        std::string_view entry_id) const;
    /// pi `getBranch`: the entries from `from_id` (default: the active
    /// leaf) up to the root, in leaf-to-root order, as snapshot copies.
    [[nodiscard]] std::vector<SessionEntry> get_branch(
        std::string_view from_id = {}) const;
    /// pi `getEntries`: every tracked entry in store order (excluding the
    /// session header), as snapshot copies.
    [[nodiscard]] std::vector<SessionEntry> entries() const;
    /// pi `getLabel`: the entry's latest label, nullopt when unset/cleared.
    [[nodiscard]] std::optional<std::string> get_label(
        std::string_view entry_id) const;
    /// pi `getSessionName`: the trimmed name of the last `session_info`
    /// entry, or nullopt when absent or blank.
    [[nodiscard]] std::optional<std::string> get_session_name() const;

    /// The JSONL session file path; empty for in-memory sessions.
    [[nodiscard]] std::optional<std::filesystem::path> path() const;
    /// The session header metadata, fixed at create/open time.
    [[nodiscard]] const SessionMetadata& metadata() const;

private:
    struct Impl;

    explicit SessionStore(std::unique_ptr<Impl> impl);

    /// The closed set of persistence alternatives.
    using StorageVariant = std::variant<JsonlSessionStore, InMemorySessionStore>;

    /// Run `fn` under the append lock against the active alternative and
    /// mirror every accepted entry into the live tree, so one append is a
    /// single persist-plus-tree step.
    [[nodiscard]] support::ExpectedVoid dispatch_append(
        std::move_only_function<support::Expected<std::vector<SessionEntry>>(StorageVariant&)> fn);

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::harness::session
