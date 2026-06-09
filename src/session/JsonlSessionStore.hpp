#pragma once

#include "../agent/Message.hpp"
#include "../util/Result.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cch::session {

struct SessionMetadata {
    std::string session_id;
    std::string created_at;
    std::filesystem::path workspace;
    std::string provider;
    std::string model;
};

struct LoadedSession {
    SessionMetadata metadata;
    std::vector<agent::Message> messages;
    std::vector<std::string> unknown_lines;
};

class JsonlSessionStore {
public:
    JsonlSessionStore() = default;

    static util::Result<JsonlSessionStore> create_new(const std::filesystem::path& path, SessionMetadata metadata);
    static util::Result<JsonlSessionStore> open_existing(const std::filesystem::path& path);
    static util::Result<LoadedSession> load(const std::filesystem::path& path);

    [[nodiscard]] util::Result<void> append(const agent::Message& message);
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] const SessionMetadata& metadata() const { return metadata_; }

private:
    static util::Result<void> validate_session_path_for_open(const std::filesystem::path& path, bool must_exist);
    static util::Result<void> ensure_private_permissions(const std::filesystem::path& path, bool existing);
    static boost::json::object header_to_json(const SessionMetadata& metadata);
    static util::Result<SessionMetadata> header_from_json(const boost::json::object& object);

    std::filesystem::path path_;
    SessionMetadata metadata_;
    std::size_t next_entry_id_{1};
};

} // namespace cch::session
