#pragma once

#include "SessionEntry.hpp"

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

    /// Get an entry by its 8-char hex ID. Returns nullopt if not found.
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

private:
    void build_index();

    SessionMetadata metadata_;
    std::vector<SessionEntry> entries_;
    std::unordered_map<std::string, std::size_t> id_to_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> children_;
};

} // namespace cch::harness::session
