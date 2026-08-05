#include "../../../include/cch/harness/session/SessionTree.hpp"

#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "SessionLeaf.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <variant>

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
    const auto* entry = getEntry(entry_id);
    if (entry != nullptr && entry->parent_id.has_value() && !entry->parent_id->empty()) {
        return entry->parent_id;
    }
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
    auto active_leaf = select_active_leaf_target(entries_);
    leaf_id_ = active_leaf.target_id.value_or("");
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

    for (const auto& entry : entries_) {
        bool has_parent = entry.parent_id.has_value() &&
                          !entry.parent_id->empty() &&
                          getEntry(*entry.parent_id) != nullptr;
        if (!has_parent) {
            return &entry;
        }
    }

    return &entries_.front();
}

// ── Context reconstruction ──

SessionContext SessionTree::buildSessionContext() const {
    SessionContext ctx;

    auto path = getBranch();
    if (path.empty()) return ctx;

    // Path is leaf-to-root. Reverse for chronological (root-to-leaf) processing.
    std::reverse(path.begin(), path.end());

    // pi `deriveSessionContextState` (packages/agent/src/harness/session/
    // session.ts): one root-to-leaf pass over the path where the last entry of
    // each kind wins, so the entry closest to the leaf is authoritative.
    // Assistant messages carry provider/model and override an earlier
    // `model_change`, exactly like pi (`entry.message.role === "assistant"`).
    for (const auto* entry : path) {
        switch (entry->kind) {
        case SessionEntryKind::ThinkingLevelChange:
            if (const auto* thinking =
                    std::get_if<ThinkingLevelChangeValue>(&entry->value)) {
                ctx.thinking_level = thinking->thinking_level;
                ctx.has_thinking_level_entry = true;
            }
            break;
        case SessionEntryKind::ModelChange:
            if (const auto* model =
                    std::get_if<ModelChangeValue>(&entry->value)) {
                ctx.provider = model->provider;
                ctx.model = model->model_id;
            }
            break;
        case SessionEntryKind::Message:
            if (entry->message.has_value()) {
                if (const auto* assistant =
                        std::get_if<ai::AssistantMessage>(&*entry->message)) {
                    ctx.provider = assistant->provider;
                    ctx.model = assistant->model;
                }
            }
            break;
        case SessionEntryKind::ActiveToolsChange:
            if (const auto* tools =
                    std::get_if<ActiveToolsChangeValue>(&entry->value)) {
                ctx.active_tool_names = tools->active_tool_names;
            }
            break;
        default:
            break;
        }
    }

    // Find the compaction entry closest to the leaf (pi's last-wins scan).
    const SessionEntry* compaction = nullptr;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if ((*it)->kind == SessionEntryKind::Compaction) {
            compaction = *it;
            break;
        }
    }

    if (compaction == nullptr) {
        // No compaction on path: emit all messages in order.
        for (const auto* entry : path) {
            emitEntryMessage(ctx, entry);
        }
        return ctx;
    }

    const auto compaction_index = static_cast<std::size_t>(
        std::find(path.begin(), path.end(), compaction) - path.begin());

    // pi `defaultContextEntryTransform`: the compaction entry plus whatever
    // follows it on the path. With `retainedTail` present the retained
    // messages live on the entry itself; without it, the pre-compaction
    // entries from `firstKeptEntryId` onward are kept in the file and emitted.
    const bool has_retained_tail = std::get_if<CompactionEntryValue>(&compaction->value) != nullptr &&
        std::get<CompactionEntryValue>(compaction->value).retained_tail.has_value();

    // Emit the compaction entry: compactionSummary + retainedTail (pi
    // `sessionEntryToContextMessages` compaction branch).
    emitCompactionMessages(ctx, compaction);

    if (has_retained_tail) {
        // Post-compaction entries only — the retained tail comes from the
        // entry, the pre-compaction history is replaced by the summary.
        for (std::size_t i = compaction_index + 1; i < path.size(); ++i) {
            emitEntryMessage(ctx, path[i]);
        }
        return ctx;
    }

    std::string first_kept_id;
    if (const auto* value = std::get_if<CompactionEntryValue>(&compaction->value)) {
        first_kept_id = value->first_kept_entry_id.value_or("");
    }

    // Collect entry IDs to skip before the first kept entry.
    std::set<std::string> skip_ids;
    bool found_kept = false;
    for (const auto* entry : path) {
        if (entry->entry_id == first_kept_id) {
            found_kept = true;
        }
        if (!found_kept) {
            skip_ids.insert(entry->entry_id);
        }
        if (entry == compaction) {
            skip_ids.erase(compaction->entry_id);
            break;
        }
    }

    // Emit messages: skip pre-kept, include kept and post-compaction.
    for (const auto* entry : path) {
        if (skip_ids.count(entry->entry_id) > 0) continue;
        emitEntryMessage(ctx, entry);
    }

    return ctx;
}

void SessionTree::emitCompactionMessages(SessionContext& ctx, const SessionEntry* entry) {
    const auto* value = std::get_if<CompactionEntryValue>(&entry->value);
    const std::string summary_text = value != nullptr ? value->summary : std::string{};
    const std::size_t tokens_before = value != nullptr ? value->tokens_before : 0;

    ai::CompactionSummaryMessage csm;
    csm.summary = summary_text;
    csm.tokens_before = static_cast<decltype(csm.tokens_before)>(tokens_before);
    csm.timestamp = entry->timestamp;
    ctx.messages.emplace_back(std::move(csm));

    if (value != nullptr && value->retained_tail.has_value()) {
        for (const auto& message : *value->retained_tail) {
            ctx.messages.push_back(message);
        }
    }
}

void SessionTree::emitEntryMessage(SessionContext& ctx, const SessionEntry* entry) {
    if (entry->kind == SessionEntryKind::Message) {
        if (entry->message.has_value()) {
            ctx.messages.push_back(*entry->message);
        }
    } else if (entry->kind == SessionEntryKind::BranchSummary) {
        // pi projects branch_summary only when it carries a summary.
        const auto* value = std::get_if<BranchSummaryEntryValue>(&entry->value);
        if (value == nullptr || value->summary.empty()) {
            return;
        }
        ai::BranchSummaryMessage bsm;
        bsm.summary = value->summary;
        bsm.from_id = value->from_id;
        bsm.timestamp = entry->timestamp;
        ctx.messages.emplace_back(std::move(bsm));
    } else if (entry->kind == SessionEntryKind::CustomMessage) {
        ai::CustomMessage cm;
        if (const auto* value = std::get_if<CustomMessageEntryValue>(&entry->value)) {
            cm.custom_type = value->custom_type;
            if (const auto* text = std::get_if<std::string>(&value->content)) {
                cm.content = {ai::TextContent{*text, std::nullopt}};
            } else {
                const auto& blocks =
                    std::get<std::vector<CustomMessageEntryContentBlock>>(value->content);
                cm.content.reserve(blocks.size());
                for (const auto& block : blocks) {
                    cm.content.push_back(std::visit(
                        [](const auto& concrete) -> ai::Content { return concrete; },
                        block));
                }
            }
            cm.display = value->display;
            cm.details = value->details;
        }
        cm.timestamp = entry->timestamp;
        ctx.messages.emplace_back(std::move(cm));
    }
}

// ── Label and session-name projection ──

std::optional<std::string> SessionTree::get_label(std::string_view entry_id) const {
    // pi `HydratedSessionState.applyProjection`: labels apply in store order,
    // later entries win; a label entry whose trimmed label is empty clears
    // (pi `const label = entry.label?.trim(); if (label) set; else delete`).
    std::optional<std::string> current;
    for (const auto& entry : entries_) {
        if (entry.kind != SessionEntryKind::Label) {
            continue;
        }
        const auto* value = std::get_if<LabelEntryValue>(&entry.value);
        if (value == nullptr || value->target_id != entry_id) {
            continue;
        }
        if (value->label.has_value()) {
            auto label = *value->label;
            const auto first = label.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                const auto last = label.find_last_not_of(" \t\r\n");
                current = label.substr(first, last - first + 1);
            } else {
                current = std::nullopt;
            }
        } else {
            current = std::nullopt;
        }
    }
    return current;
}

std::optional<std::string> SessionTree::get_session_name() const {
    // pi `getSessionName`: scan from the end, return the last `session_info`
    // entry's trimmed name when non-empty.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->kind != SessionEntryKind::SessionInfo) {
            continue;
        }
        const auto* value = std::get_if<SessionInfoEntryValue>(&it->value);
        if (value == nullptr || !value->name.has_value()) {
            continue;
        }
        auto trimmed = *value->name;
        // pi trims surrounding whitespace (and its append path already
        // collapses newlines); mirror the trim here.
        const auto first = trimmed.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        const auto last = trimmed.find_last_not_of(" \t\r\n");
        return trimmed.substr(first, last - first + 1);
    }
    return std::nullopt;
}

// ── Branch summary hook ──

util::ExpectedVoid SessionTree::branchWithSummary(
    std::string_view entry_id,
    BranchSummaryHook& hook,
    std::move_only_function<util::ExpectedVoid(const SessionEntry&)> append_writer) {

    if (!getEntry(entry_id)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Session,
            std::string("target entry not found: ") + std::string{entry_id}));
    }

    // Collect the abandoned branch.
    auto from_path = getBranch(leaf_id_);
    auto to_path = getBranch(entry_id);

    BranchSummaryContext ctx;
    ctx.from_leaf_id = leaf_id_;
    ctx.to_entry_id = std::string{entry_id};

    // branch_entries: entries on from_path up to common ancestor.
    for (const auto* entry : from_path) {
        bool in_to_path = false;
        for (const auto* te : to_path) {
            if (te->entry_id == entry->entry_id) {
                in_to_path = true;
                break;
            }
        }
        if (in_to_path) break;
        ctx.branch_entries.push_back(entry);
    }

    // Call the hook.
    auto summary_result = hook(ctx);
    if (!summary_result.has_value()) {
        return std::unexpected(summary_result.error());
    }

    if (summary_result->has_value()) {
        const auto& data = **summary_result;

        SessionEntry summary_entry;
        summary_entry.kind = SessionEntryKind::BranchSummary;
        summary_entry.parent_id = std::string{entry_id};
        summary_entry.value = BranchSummaryEntryValue{
            .from_id = std::string{ctx.from_leaf_id},
            .summary = data.summary,
            .details = data.details,
            .usage = std::nullopt,
            .from_hook = std::nullopt,
        };

        auto write_result = append_writer(summary_entry);
        if (!write_result.has_value()) {
            return write_result;
        }
    }

    return branch(entry_id);
}

} // namespace cch::harness::session
