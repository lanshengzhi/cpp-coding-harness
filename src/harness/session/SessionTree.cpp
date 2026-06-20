#include "../../../include/cch/harness/session/SessionTree.hpp"

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
        previous_id = entry.entry_id;
    }
}

const SessionEntry* SessionTree::getEntry(std::string_view entry_id) const {
    auto it = id_to_index_.find(std::string{entry_id});
    if (it == id_to_index_.end()) return nullptr;
    return &entries_[it->second];
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

} // namespace cch::harness::session
