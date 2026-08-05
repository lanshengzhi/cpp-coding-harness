#pragma once

#include "../../ai/Message.hpp"
#include "../../util/JsonValue.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::harness::session {

struct SessionMetadata {
    std::string session_id;
    std::string created_at;
    std::filesystem::path workspace;
    std::string provider;
    std::string model;
};

enum class SessionEntryKind {
    Header,
    Message,
    ModelChange,
    ThinkingLevelChange,
    ActiveToolsChange,
    Custom,
    CustomMessage,
    Label,
    Compaction,
    BranchSummary,
    SessionInfo,
    Leaf,
    Unknown,
};

struct ModelChangeValue {
    std::string provider;
    std::string model_id;
};

struct ThinkingLevelChangeValue {
    std::string thinking_level;
};

struct ActiveToolsChangeValue {
    std::vector<std::string> active_tool_names;
};

struct CustomEntryValue {
    std::string custom_type;
    /// pi `data?: T` — absent (omitted on the wire) vs explicit null are both
    /// representable: nullopt = absent, engaged = the JSON value (including an
    /// explicit JSON null).
    std::optional<util::JsonValue> data;
};

using CustomMessageEntryContentBlock = std::variant<ai::TextContent, ai::ImageContent>;
using CustomMessageEntryContent =
    std::variant<std::string, std::vector<CustomMessageEntryContentBlock>>;

struct CustomMessageEntryValue {
    std::string custom_type;
    CustomMessageEntryContent content;
    bool display{true};
    std::optional<util::JsonValue> details;
};

struct LabelEntryValue {
    std::string target_id;
    std::optional<std::string> label;
};

struct CompactionEntryValue {
    std::string summary;
    /// pi `firstKeptEntryId?: string` — nullopt is omitted on the wire.
    std::optional<std::string> first_kept_entry_id;
    std::size_t tokens_before{0};
    /// pi `retainedTail?: AgentMessage[]` — recent messages kept on the entry
    /// itself; context rebuild projects compactionSummary + retained tail.
    std::optional<std::vector<ai::MessageVariant>> retained_tail;
    std::optional<util::JsonValue> details;
    std::optional<ai::Usage> usage;
    std::optional<bool> from_hook;
};

struct BranchSummaryEntryValue {
    std::string from_id;
    std::string summary;
    std::optional<util::JsonValue> details;
    std::optional<ai::Usage> usage;
    std::optional<bool> from_hook;
};

struct SessionInfoEntryValue {
    /// pi `name?: string` — nullopt is omitted on the wire.
    std::optional<std::string> name;
};

struct LeafEntryValue {
    std::optional<std::string> target_id;
};

using SessionEntryValue = std::variant<
    std::monostate,
    ModelChangeValue,
    ThinkingLevelChangeValue,
    ActiveToolsChangeValue,
    CustomEntryValue,
    CustomMessageEntryValue,
    LabelEntryValue,
    CompactionEntryValue,
    BranchSummaryEntryValue,
    SessionInfoEntryValue,
    LeafEntryValue>;

struct SessionEntry {
    SessionEntryKind kind{SessionEntryKind::Unknown};
    std::string entry_id;
    std::optional<std::string> parent_id;
    std::optional<std::string> leaf_id;
    ai::TimestampMs timestamp{};
    std::optional<ai::MessageVariant> message;
    SessionEntryValue value;
    util::JsonValue payload;
    std::string raw_line;
};

struct LoadedSession {
    SessionMetadata metadata;
    std::vector<ai::MessageVariant> messages;
    std::vector<SessionEntry> entries;
    std::vector<std::string> unknown_lines;
};

} // namespace cch::harness::session
