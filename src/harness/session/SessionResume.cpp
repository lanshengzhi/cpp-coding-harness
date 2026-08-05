#include "../../../include/cch/harness/session/SessionResume.hpp"

#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/harness/session/SessionTree.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace cch::harness::session {
namespace {

[[nodiscard]] bool participates_in_topology(const SessionEntry& entry) {
    switch (entry.kind) {
    case SessionEntryKind::Message:
    case SessionEntryKind::ModelChange:
    case SessionEntryKind::ThinkingLevelChange:
    case SessionEntryKind::ActiveToolsChange:
    case SessionEntryKind::Custom:
    case SessionEntryKind::CustomMessage:
    case SessionEntryKind::Compaction:
    case SessionEntryKind::BranchSummary:
        return true;
    case SessionEntryKind::Header:
    case SessionEntryKind::Label:
    case SessionEntryKind::SessionInfo:
    case SessionEntryKind::Leaf:
    case SessionEntryKind::Unknown:
        return false;
    }
    return false;
}

[[nodiscard]] std::size_t entry_index(
    const std::vector<SessionEntry>& entries,
    const SessionEntry* entry) {
    if (entries.empty() || entry == nullptr) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(entry - entries.data());
}

[[nodiscard]] const SessionEntry* primary_topology_child(
    const SessionTree& tree,
    const SessionEntry& parent) {
    const auto& entries = tree.entries();
    const SessionEntry* primary = nullptr;
    auto primary_index = std::numeric_limits<std::size_t>::max();

    for (const auto* child : tree.getChildren(parent.entry_id)) {
        if (child == nullptr || !participates_in_topology(*child)) {
            continue;
        }
        const auto index = entry_index(entries, child);
        if (index < primary_index) {
            primary = child;
            primary_index = index;
        }
    }

    return primary;
}

[[nodiscard]] SessionTopology classify_active_topology(const SessionTree& tree) {
    auto path = tree.getBranch();
    if (path.empty()) {
        return SessionTopology::Linear;
    }

    std::reverse(path.begin(), path.end());

    for (const auto* entry : path) {
        if (entry->kind == SessionEntryKind::Compaction) {
            return SessionTopology::Compacted;
        }
        if (entry->kind == SessionEntryKind::BranchSummary) {
            return SessionTopology::Branched;
        }
    }

    for (std::size_t i = 1; i < path.size(); ++i) {
        const auto* primary = primary_topology_child(tree, *path[i - 1]);
        if (primary != nullptr && primary->entry_id != path[i]->entry_id) {
            return SessionTopology::Branched;
        }
    }

    const auto* leaf = path.back();
    const auto* primary = primary_topology_child(tree, *leaf);
    if (primary != nullptr) {
        return SessionTopology::Branched;
    }

    return SessionTopology::Linear;
}

} // namespace

util::Expected<SessionResumeResult> resume_session(const std::filesystem::path& path) {
    auto loaded = JsonlSessionStore::load(path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    SessionTree tree(std::move(*loaded));
    auto context = tree.buildSessionContext();

    return SessionResumeResult{
        .metadata = tree.metadata(),
        .history = std::move(context.messages),
        .provider = std::move(context.provider),
        .model = std::move(context.model),
        .thinking_level = std::move(context.thinking_level),
        .has_thinking_level_entry = context.has_thinking_level_entry,
        .topology = classify_active_topology(tree),
    };
}

} // namespace cch::harness::session
