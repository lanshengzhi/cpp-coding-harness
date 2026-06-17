#pragma once

#include "../../ai/Message.hpp"
#include "../../util/JsonValue.hpp"

#include <filesystem>
#include <optional>
#include <string>
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

struct SessionEntry {
    SessionEntryKind kind{SessionEntryKind::Unknown};
    std::string entry_id;
    std::optional<std::string> parent_id;
    std::optional<std::string> leaf_id;
    std::optional<ai::MessageVariant> message;
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
