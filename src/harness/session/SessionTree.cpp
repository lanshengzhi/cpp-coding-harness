#include "../../../include/cch/harness/session/SessionTree.hpp"

#include "../../../include/cch/util/Error.hpp"

namespace cch::harness::session {

SessionTree::SessionTree(LoadedSession session)
    : metadata_(std::move(session.metadata)) {
    // Filter out the session header entry (kind == Header) and any Unknown entries.
    // The tree only tracks non-header, non-unknown entries.
    for (auto& entry : session.entries) {
        if (entry.kind == SessionEntryKind::Header ||
            entry.kind == SessionEntryKind::Unknown) {
            continue;
        }
        entries_.push_back(std::move(entry));
    }
    build_index();
    restore_leaf_position();
}

void SessionTree::build_index() {
    std::optional<std::string> previous_id;

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];

        // Index by entry ID for O(1) lookup.
        if (!entry.entry_id.empty()) {
            id_to_index_[entry.entry_id] = i;
        }

        // Determine parent: use explicit parent_id if set, otherwise infer
        // from the previous entry's ID (linear chain).
        std::string effective_parent;
        if (entry.parent_id.has_value() && !entry.parent_id->empty()) {
            effective_parent = *entry.parent_id;
        } else if (previous_id.has_value()) {
            effective_parent = *previous_id;
        } else {
            // First entry with no explicit parent: root, no children entry.
            previous_id = entry.entry_id;
            continue;
        }

        children_[effective_parent].push_back(i);

        // Record the effective parent for leaf-to-root traversal.
        // This may differ from entry.parent_id when inferred from linear order.
        if (!entry.parent_id.has_value() || entry.parent_id->empty()) {
            inferred_parent_[entry.entry_id] = effective_parent;
        }

        previous_id = entry.entry_id;
    }
}

const SessionEntry* SessionTree::getEntry(std::string_view entry_id) const {
    auto it = id_to_index_.find(std::string{entry_id});
    if (it == id_to_index_.end()) return nullptr;
    return &entries_[it->second];
}

std::optional<std::string> SessionTree::effective_parent_id(std::string_view entry_id) const {
    // Prefer explicit parent_id from the entry.
    const auto* entry = getEntry(entry_id);
    if (entry != nullptr && entry->parent_id.has_value() && !entry->parent_id->empty()) {
        return entry->parent_id;
    }
    // Fall back to inferred parent from linear ordering.
    auto it = inferred_parent_.find(std::string{entry_id});
    if (it != inferred_parent_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<const SessionEntry*> SessionTree::getChildren(std::string_view parent_id) const {
    auto it = children_.find(std::string{parent_id});
    if (it == children_.end()) return {};
    std::vector<const SessionEntry*> result;
    result.reserve(it->second.size());
    for (std::size_t idx : it->second) {
        result.push_back(&entries_[idx]);
    }
    return result;
}

// ── Leaf navigation ──

void SessionTree::restore_leaf_position() {
    // Scan entries in reverse for the last Leaf entry.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->kind == SessionEntryKind::Leaf) {
            // Extract targetId from the payload.
            if (const auto* obj = it->payload.get_if<util::JsonValue::object_t>()) {
                auto target = obj->find("targetId");
                if (target != obj->end()) {
                    if (const auto* target_str = target->second.get_if<std::string>()) {
                        // Verify the target exists in the tree.
                        if (getEntry(*target_str) != nullptr) {
                            leaf_id_ = *target_str;
                            return;
                        }
                    }
                }
            }
        }
    }

    // No Leaf entry found or target invalid — default to the last entry.
    if (!entries_.empty()) {
        leaf_id_ = entries_.back().entry_id;
    }
}

const SessionEntry* SessionTree::leaf_entry() const {
    if (leaf_id_.empty()) return nullptr;
    return getEntry(leaf_id_);
}

util::ExpectedVoid SessionTree::branch(std::string_view entry_id) {
    if (!getEntry(entry_id)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Session,
            std::string("entry not found: ") + std::string{entry_id}));
    }
    leaf_id_ = std::string{entry_id};
    return {};
}

std::vector<const SessionEntry*> SessionTree::getBranch(std::string_view from_id) const {
    const SessionEntry* start = nullptr;

    if (from_id.empty()) {
        start = leaf_entry();
    } else {
        start = getEntry(from_id);
    }

    if (start == nullptr) return {};

    // Walk leaf-to-root following parent chain.
    std::vector<const SessionEntry*> path;
    const SessionEntry* current = start;
    while (current != nullptr) {
        path.push_back(current);
        auto parent = effective_parent_id(current->entry_id);
        if (parent.has_value()) {
            current = getEntry(*parent);
        } else {
            break;
        }
    }

    return path;
}

const SessionEntry* SessionTree::root() const {
    if (entries_.empty()) return nullptr;

    // Find the entry with no valid parent.
    for (const auto& entry : entries_) {
        bool has_parent = entry.parent_id.has_value() &&
                          !entry.parent_id->empty() &&
                          getEntry(*entry.parent_id) != nullptr;
        if (!has_parent) {
            return &entry;
        }
    }

    // Fallback: first entry.
    return &entries_.front();
}

} // namespace cch::harness::session
