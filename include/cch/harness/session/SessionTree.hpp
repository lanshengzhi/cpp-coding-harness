#pragma once

#include "SessionEntry.hpp"
#include "../../ai/Message.hpp"
#include "../../util/Error.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cch::harness::session {

/// Result of context reconstruction from a session tree path.
struct SessionContext {
    /// Messages ready for LLM consumption, in chronological order.
    std::vector<ai::MessageVariant> messages;
    /// Current model provider derived from the last `model_change` or assistant
    /// message on the path (pi `model: {provider, modelId} | null`); nullopt
    /// when the branch carries neither.
    std::optional<std::string> provider;
    /// Current model id derived from the last `model_change` or assistant
    /// message on the path.
    std::optional<std::string> model;
    /// pi `thinkingLevel` derived state: `"off"` by default, updated by the
    /// last `thinking_level_change` on the path (pi `deriveSessionContextState`).
    std::string thinking_level{"off"};
    /// True when the path carries a `thinking_level_change` entry (pi sdk.ts
    /// `hasThinkingEntry` gates resumed-level restoration: an entry wins over
    /// the settings `defaultThinkingLevel`).
    bool has_thinking_level_entry{false};
    /// pi `activeToolNames: string[] | null`: a copy of the last
    /// `active_tools_change` on the path; nullopt when the branch carries none.
    std::optional<std::vector<std::string>> active_tool_names;
};

/// In-memory session tree index and navigation capability.
///
/// Constructed from a LoadedSession, SessionTree builds a hash-based index
/// (entry ID → position) and a children map (parent ID → child positions)
/// for O(1) entry lookup and efficient tree traversal.
///
/// SessionTree is move-only — it owns the entry vector and indices.
class SessionTree {
public:
    /// Construct from a loaded session's entries.
    /// Indexes entries by ID and parent relationships on construction.
    explicit SessionTree(LoadedSession session);

    SessionTree(SessionTree&&) = default;
    SessionTree& operator=(SessionTree&&) = default;
    SessionTree(const SessionTree&) = delete;
    SessionTree& operator=(const SessionTree&) = delete;

    // ── Basic queries ──

    /// Get an entry by its 8-char hex ID. Returns nullptr if not found.
    [[nodiscard]] const SessionEntry* getEntry(std::string_view entry_id) const;

    /// Get direct children of a parent entry.
    [[nodiscard]] std::vector<const SessionEntry*> getChildren(std::string_view parent_id) const;

    /// All entries in store order (excluding the session header).
    [[nodiscard]] const std::vector<SessionEntry>& entries() const { return entries_; }

    /// Session metadata.
    [[nodiscard]] const SessionMetadata& metadata() const { return metadata_; }

    /// Count of entries (excluding header).
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    /// True if the tree has no entries (header-only session).
    [[nodiscard]] bool empty() const { return entries_.empty(); }

    // ── Leaf navigation ──

    /// Current active leaf entry ID. Empty string if tree is empty.
    [[nodiscard]] const std::string& leaf_id() const { return leaf_id_; }

    /// Current active leaf entry. Returns nullptr if tree is empty.
    [[nodiscard]] const SessionEntry* leaf_entry() const;

    /// Move the active leaf to a different entry.
    /// The target entry_id must exist in the tree.
    /// Returns an error if entry_id is not found.
    [[nodiscard]] util::ExpectedVoid branch(std::string_view entry_id);

    /// Collect entries from a given entry up to the root (leaf-to-root order).
    /// If from_id is empty, starts from the current leaf.
    [[nodiscard]] std::vector<const SessionEntry*> getBranch(std::string_view from_id = {}) const;

    /// Get the root entry (the one with no parent).
    /// Returns nullptr if tree is empty.
    [[nodiscard]] const SessionEntry* root() const;

    // ── Context reconstruction ──

    /// Reconstruct LLM context from the current leaf path.
    /// Walks leaf-to-root, handles CompactionEntry (summary→kept messages→
    /// post-compaction, including pi's `retainedTail` projection), converts
    /// BranchSummaryEntry/CustomMessageEntry to message types, and derives
    /// thinkingLevel/model/activeToolNames per pi `deriveSessionContextState`
    /// (last entry of each kind wins; assistant messages carry provider/model).
    /// Custom entries are omitted from model context by default (pi
    /// `sessionEntryToContextMessages`).
    [[nodiscard]] SessionContext buildSessionContext() const;

    // ── Label and session-name projection ──

    /// pi `getLabel`: the label of the most recent label entry targeting
    /// `entry_id` (a later label with an empty/absent label clears it).
    [[nodiscard]] std::optional<std::string> get_label(std::string_view entry_id) const;

    /// pi `getSessionName`: the trimmed name of the last `session_info` entry,
    /// or nullopt when absent or blank.
    [[nodiscard]] std::optional<std::string> get_session_name() const;

    // ── Branch summary hook ──

    /// Context passed to a branch summary hook when switching branches.
    struct BranchSummaryContext {
        /// Leaf ID of the branch being abandoned.
        std::string from_leaf_id;
        /// Target entry ID being navigated to.
        std::string to_entry_id;
        /// Entries on the abandoned branch (from old leaf to common ancestor).
        std::vector<const SessionEntry*> branch_entries;
    };

    /// Data produced by a branch summary hook.
    struct BranchSummaryData {
        std::string summary;
        std::optional<util::JsonValue> details;
    };

    /// Hook for generating branch summaries.
    /// Returns BranchSummaryData on success, nullopt to skip summary,
    /// or an error on failure.
    using BranchSummaryHook = std::move_only_function<
        util::Expected<std::optional<BranchSummaryData>>(const BranchSummaryContext&)>;

    /// Navigate to a target entry and optionally generate a branch summary.
    /// If hook is provided and returns data, a BranchSummary entry is appended.
    /// The append_writer callback writes entries to the underlying store.
    [[nodiscard]] util::ExpectedVoid branchWithSummary(
        std::string_view entry_id,
        BranchSummaryHook& hook,
        std::move_only_function<util::ExpectedVoid(const SessionEntry&)> append_writer);

private:
    void build_index();

    void restore_leaf_position();

    /// Get the effective parent ID for leaf-to-root traversal.
    /// Prefers explicit parent_id, falls back to inferred parent from linear ordering.
    [[nodiscard]] std::optional<std::string> effective_parent_id(std::string_view entry_id) const;

    SessionMetadata metadata_;
    std::vector<SessionEntry> entries_;
    std::string leaf_id_;
    std::unordered_map<std::string, std::size_t> id_to_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> children_;
    /// Inferred parent relationships for entries without explicit parent_id.
    std::unordered_map<std::string, std::string> inferred_parent_;
};

/// Reconstruct LLM context from a root-to-leaf entry path without a
/// SessionTree (pi `buildSessionContext(pathEntries)` in
/// harness/session/session.ts, which the compaction machinery also calls to
/// estimate `tokensBefore`). The SessionTree member function delegates to
/// this free function with the tree's leaf path.
[[nodiscard]] SessionContext buildSessionContext(
    const std::vector<const SessionEntry*>& path);

} // namespace cch::harness::session
