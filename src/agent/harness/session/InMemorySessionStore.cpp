#include "InMemorySessionStore.hpp"

#include "agent/harness/session/EntrySerializer.hpp"

#include <utility>

namespace cch::harness::session {

namespace {

/// One freshly minted entry of the given kind: generated id and timestamp,
/// explicit wire parent (nullopt is the root / linear-chain position), no
/// persisted line.
[[nodiscard]] SessionEntry make_entry(
    SessionEntryKind kind,
    std::optional<std::string> parent_id) {
    SessionEntry entry;
    entry.kind = kind;
    entry.entry_id = EntrySerializer::new_entry_id();
    entry.parent_id = std::move(parent_id);
    entry.timestamp = EntrySerializer::now_timestamp_ms();
    return entry;
}

[[nodiscard]] support::Expected<std::vector<SessionEntry>> single(SessionEntry entry) {
    std::vector<SessionEntry> entries;
    entries.push_back(std::move(entry));
    return entries;
}

} // namespace

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append(
    const ai::MessageVariant& message,
    std::optional<std::string> parent_id) {
    auto entry = make_entry(SessionEntryKind::Message, std::move(parent_id));
    entry.message = message;
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    auto entry = make_entry(SessionEntryKind::ModelChange, std::move(parent_id));
    entry.value = ModelChangeValue{
        .provider = std::move(provider),
        .model_id = std::move(model_id),
    };
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    auto entry = make_entry(SessionEntryKind::ThinkingLevelChange, std::move(parent_id));
    entry.value = ThinkingLevelChangeValue{.thinking_level = std::move(thinking_level)};
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    auto entry = make_entry(SessionEntryKind::Label, std::move(parent_id));
    entry.value = LabelEntryValue{
        .target_id = std::move(target_id),
        .label = std::move(label),
    };
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_compaction(
    std::optional<std::string> parent_id,
    std::string summary,
    std::string first_kept_entry_id,
    std::size_t tokens_before,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook,
    std::vector<ai::MessageVariant> retained_tail,
    std::optional<ai::Usage> usage) {
    auto entry = make_entry(SessionEntryKind::Compaction, std::move(parent_id));
    entry.value = CompactionEntryValue{
        .summary = std::move(summary),
        .first_kept_entry_id = std::move(first_kept_entry_id),
        .tokens_before = tokens_before,
        // Match the wire shape: an empty retained tail is omitted (nullopt),
        // which keeps context rebuild on the firstKeptEntryId path.
        .retained_tail = retained_tail.empty()
            ? std::nullopt
            : std::optional<std::vector<ai::MessageVariant>>{std::move(retained_tail)},
        .details = std::move(details),
        .usage = std::move(usage),
        .from_hook = from_hook,
    };
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook) {
    auto entry = make_entry(SessionEntryKind::BranchSummary, std::move(parent_id));
    entry.value = BranchSummaryEntryValue{
        .from_id = std::move(from_id),
        .summary = std::move(summary),
        .details = std::move(details),
        .usage = std::nullopt,
        .from_hook = from_hook,
    };
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    auto entry = make_entry(SessionEntryKind::SessionInfo, std::move(parent_id));
    entry.value = SessionInfoEntryValue{.name = std::move(name)};
    return single(std::move(entry));
}

support::Expected<std::vector<SessionEntry>> InMemorySessionStore::append_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) {
    auto entry = make_entry(SessionEntryKind::Leaf, std::move(parent_id));
    entry.value = LeafEntryValue{.target_id = std::move(target_id)};
    return single(std::move(entry));
}

std::optional<std::filesystem::path> InMemorySessionStore::path() const {
    return std::nullopt;
}

} // namespace cch::harness::session
