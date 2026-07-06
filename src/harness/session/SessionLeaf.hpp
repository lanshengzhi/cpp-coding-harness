#pragma once

#include "../../../include/cch/harness/session/SessionEntry.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace cch::harness::session {

struct LeafTargetSelection {
    std::optional<std::string> target_id;
    bool saw_leaf_marker{false};
};

[[nodiscard]] inline bool is_navigable_leaf_target(const SessionEntry& entry) {
    return entry.kind != SessionEntryKind::Header &&
           entry.kind != SessionEntryKind::Unknown &&
           entry.kind != SessionEntryKind::Leaf &&
           !entry.entry_id.empty();
}

[[nodiscard]] inline LeafTargetSelection select_active_leaf_target(const std::vector<SessionEntry>& entries) {
    std::unordered_set<std::string> navigable_ids;
    std::optional<std::string> last_navigable_id;
    for (const auto& entry : entries) {
        if (!is_navigable_leaf_target(entry)) {
            continue;
        }
        navigable_ids.insert(entry.entry_id);
        last_navigable_id = entry.entry_id;
    }

    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->kind != SessionEntryKind::Leaf) {
            continue;
        }

        const auto* leaf = std::get_if<LeafEntryValue>(&it->value);
        if (leaf != nullptr && !leaf->target_id.has_value()) {
            return LeafTargetSelection{std::nullopt, true};
        }
        if (leaf != nullptr && navigable_ids.contains(*leaf->target_id)) {
            return LeafTargetSelection{leaf->target_id, true};
        }
        return LeafTargetSelection{last_navigable_id, true};
    }

    return LeafTargetSelection{last_navigable_id, false};
}

} // namespace cch::harness::session
