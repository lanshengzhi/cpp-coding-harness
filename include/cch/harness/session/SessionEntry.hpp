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
    util::JsonValue data;
};

struct CustomMessageEntryValue {
    std::string custom_type;
    std::string content;
    bool display{true};
    std::optional<util::JsonValue> details;
};

struct LabelEntryValue {
    std::string target_id;
    std::optional<std::string> label;
};

struct CompactionEntryValue {
    std::string summary;
    std::string first_kept_entry_id;
    std::size_t tokens_before{0};
    std::optional<util::JsonValue> details;
    std::optional<bool> from_hook;
};

struct BranchSummaryEntryValue {
    std::string from_id;
    std::string summary;
    std::optional<util::JsonValue> details;
    std::optional<bool> from_hook;
};

struct SessionInfoEntryValue {
    std::string name;
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
