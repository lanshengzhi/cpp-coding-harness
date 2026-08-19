#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cstddef>
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
    std::optional<std::size_t> last_navigable_index;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (!is_navigable_leaf_target(entry)) {
            continue;
        }
        navigable_ids.insert(entry.entry_id);
        last_navigable_id = entry.entry_id;
        last_navigable_index = index;
    }

    for (std::size_t index = entries.size(); index > 0; --index) {
        const auto& entry = entries[index - 1];
        if (entry.kind != SessionEntryKind::Leaf) {
            continue;
        }

        // A durable topology entry after the latest leaf marker is a newer
        // resume point. This occurs when a message write succeeds but the
        // following leaf-marker write fails.
        if (last_navigable_index && *last_navigable_index > index - 1) {
            return LeafTargetSelection{last_navigable_id, true};
        }

        const auto* leaf = std::get_if<LeafEntryValue>(&entry.value);
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
