#pragma once

#include "SessionEntry.hpp"

#include <cch/util/Error.hpp>

#include <filesystem>
#include <string>

namespace cch::harness::session {

class JsonlSessionStore {
public:
    JsonlSessionStore() = default;

    static util::Expected<JsonlSessionStore> create_new(const std::filesystem::path& path, SessionMetadata metadata);
    static util::Expected<JsonlSessionStore> open_existing(const std::filesystem::path& path);
    static util::Expected<LoadedSession> load(const std::filesystem::path& path);

    [[nodiscard]] util::ExpectedVoid append(const ai::MessageVariant& message);
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] const SessionMetadata& metadata() const { return metadata_; }

private:
    static util::ExpectedVoid validate_session_path_for_open(const std::filesystem::path& path, bool must_exist);
    static util::ExpectedVoid ensure_private_permissions(const std::filesystem::path& path, bool existing);

    std::filesystem::path path_;
    SessionMetadata metadata_;
    std::size_t next_entry_id_{1};
};

} // namespace cch::harness::session
