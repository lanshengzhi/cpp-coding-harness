#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

/// pi `SessionManager._buildIndex`: the active leaf is the LAST entry in the
/// file (`this.leafId = entry.id` for every non-header entry, in file order).
/// pi keeps that position in memory only — it is never written to the file —
/// so this selection is the load-time reconstruction of a runtime-only
/// value, not a persisted marker.
[[nodiscard]] inline std::optional<std::string> select_active_leaf_id(const std::vector<SessionEntry>& entries) {
    std::optional<std::string> leaf_id;
    for (const auto& entry : entries) {
        if (entry.kind == SessionEntryKind::Header) {
            continue;
        }
        leaf_id = entry.entry_id;
    }
    return leaf_id;
}

/// The entries that can carry the active position: everything except the
/// header and the unknown foreign lines (which pi's load loop still counts,
/// but the C++ tree does not track).
[[nodiscard]] inline bool is_navigable_leaf_target(const SessionEntry& entry) {
    return entry.kind != SessionEntryKind::Header &&
           entry.kind != SessionEntryKind::Unknown &&
           !entry.entry_id.empty();
}

} // namespace cch::harness::session
