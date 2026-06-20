#pragma once

#include "SessionEntry.hpp"
#include "../../util/Error.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cch::harness::session {

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

} // namespace cch::harness::session
