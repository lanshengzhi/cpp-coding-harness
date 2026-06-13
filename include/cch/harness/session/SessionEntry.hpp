#pragma once

#include <cch/ai/Message.hpp>

#include <filesystem>
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
    Unknown,
};

struct SessionEntry {
    SessionEntryKind kind{SessionEntryKind::Unknown};
    std::string entry_id;
    ai::MessageVariant message;
    std::string raw_line;
};

struct LoadedSession {
    SessionMetadata metadata;
    std::vector<ai::MessageVariant> messages;
    std::vector<SessionEntry> entries;
    std::vector<std::string> unknown_lines;
};

} // namespace cch::harness::session
