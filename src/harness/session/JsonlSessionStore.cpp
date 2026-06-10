#include "JsonlSessionStore.hpp"

#include <boost/json.hpp>

#include <iomanip>
#include <sstream>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cch::harness::session {
namespace {

util::Result<boost::json::object> parse_line(const std::string& line, std::size_t line_number) {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(line, ec);
    if (ec || !parsed.is_object()) {
        return util::Result<boost::json::object>::failure("malformed JSONL at line " + std::to_string(line_number));
    }
    return util::Result<boost::json::object>::success(parsed.as_object());
}

bool has_public_read(std::filesystem::perms mode) {
    using std::filesystem::perms;
    return (mode & perms::others_read) != perms::none || (mode & perms::group_read) != perms::none;
}

util::Result<void> write_new_file_exclusive(const std::filesystem::path& path, const std::string& content) {
#if defined(__unix__) || defined(__APPLE__)
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        return util::Result<void>::failure("could not create session file: " + std::string(std::strerror(errno)));
    }
    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            const auto message = std::string(std::strerror(errno));
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return util::Result<void>::failure("could not write session header: " + message);
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return util::Result<void>::failure("could not flush session header: " + message);
    }
    if (::close(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return util::Result<void>::failure("could not close session file: " + message);
    }
    return util::Result<void>::success();
#else
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return util::Result<void>::failure("session file already exists; use resume to append");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return util::Result<void>::failure("could not create session file");
    }
    output << content;
    output.flush();
    output.close();
    if (!output) {
        return util::Result<void>::failure("could not write session header");
    }
    return util::Result<void>::success();
#endif
}

} // namespace

util::Result<JsonlSessionStore> JsonlSessionStore::create_new(const std::filesystem::path& path, SessionMetadata metadata) {
    auto validation = validate_session_path_for_open(path, false);
    if (!validation) {
        return util::Result<JsonlSessionStore>::failure(validation.error());
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return util::Result<JsonlSessionStore>::failure("could not create session directory: " + ec.message());
        }
    }
    if (std::filesystem::exists(path, ec)) {
        return util::Result<JsonlSessionStore>::failure("session file already exists; use resume to append");
    }
    auto header = write_new_file_exclusive(path, boost::json::serialize(header_to_json(metadata)) + '\n');
    if (!header) {
        return util::Result<JsonlSessionStore>::failure(header.error());
    }
    if (auto perms = ensure_private_permissions(path, false); !perms) {
        return util::Result<JsonlSessionStore>::failure(perms.error());
    }
    JsonlSessionStore store;
    store.path_ = path;
    store.metadata_ = std::move(metadata);
    return util::Result<JsonlSessionStore>::success(std::move(store));
}

util::Result<JsonlSessionStore> JsonlSessionStore::open_existing(const std::filesystem::path& path) {
    auto loaded = load(path);
    if (!loaded) {
        return util::Result<JsonlSessionStore>::failure(loaded.error());
    }
    JsonlSessionStore store;
    store.path_ = path;
    store.metadata_ = loaded.value().metadata;
    store.next_entry_id_ = loaded.value().messages.size() + 1;
    return util::Result<JsonlSessionStore>::success(std::move(store));
}

util::Result<LoadedSession> JsonlSessionStore::load(const std::filesystem::path& path) {
    auto validation = validate_session_path_for_open(path, true);
    if (!validation) {
        return util::Result<LoadedSession>::failure(validation.error());
    }
    if (auto perms = ensure_private_permissions(path, true); !perms) {
        return util::Result<LoadedSession>::failure(perms.error());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return util::Result<LoadedSession>::failure("could not open session file");
    }

    LoadedSession loaded;
    std::string line;
    std::size_t line_number = 0;
    bool saw_header = false;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        auto object = parse_line(line, line_number);
        if (!object) {
            return util::Result<LoadedSession>::failure(object.error());
        }
        auto* type = object.value().if_contains("type");
        if (type == nullptr || !type->is_string()) {
            return util::Result<LoadedSession>::failure("session entry missing type at line " + std::to_string(line_number));
        }
        const std::string type_string(type->as_string());
        if (line_number == 1 && type_string == "header") {
            auto header = header_from_json(object.value());
            if (!header) {
                return util::Result<LoadedSession>::failure(header.error());
            }
            loaded.metadata = header.value();
            SessionEntry entry;
            entry.kind = SessionEntryKind::Header;
            entry.raw_line = line;
            loaded.entries.push_back(std::move(entry));
            saw_header = true;
            continue;
        }
        if (type_string == "message") {
            auto* message_value = object.value().if_contains("message");
            if (message_value == nullptr || !message_value->is_object()) {
                return util::Result<LoadedSession>::failure("message entry missing message at line " + std::to_string(line_number));
            }
            auto message = agent::message_from_json(message_value->as_object());
            if (!message) {
                return util::Result<LoadedSession>::failure("invalid message at line " + std::to_string(line_number) + ": " + message.error());
            }
            SessionEntry entry;
            entry.kind = SessionEntryKind::Message;
            if (auto* entry_id = object.value().if_contains("entry_id"); entry_id && entry_id->is_string()) {
                entry.entry_id = std::string(entry_id->as_string());
            }
            entry.message = message.value();
            entry.raw_line = line;
            loaded.messages.push_back(message.value());
            loaded.entries.push_back(std::move(entry));
        } else {
            SessionEntry entry;
            entry.kind = SessionEntryKind::Unknown;
            entry.raw_line = line;
            loaded.entries.push_back(std::move(entry));
            loaded.unknown_lines.push_back(line);
        }
    }
    if (!saw_header) {
        return util::Result<LoadedSession>::failure("session header is missing");
    }
    return util::Result<LoadedSession>::success(std::move(loaded));
}

util::Result<void> JsonlSessionStore::append(const agent::Message& message) {
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) {
        return util::Result<void>::failure("could not append to session file");
    }
    agent::Message redacted = agent::redact_message(message);
    boost::json::object entry;
    entry["type"] = "message";
    entry["entry_id"] = "m" + std::to_string(next_entry_id_);
    entry["message"] = agent::message_to_json(redacted);
    output << boost::json::serialize(entry) << '\n';
    output.flush();
    output.close();
    if (!output) {
        return util::Result<void>::failure("could not persist session entry");
    }
    ++next_entry_id_;
    return util::Result<void>::success();
}

util::Result<void> JsonlSessionStore::validate_session_path_for_open(const std::filesystem::path& path, bool must_exist) {
    std::error_code ec;
    if (path.empty()) {
        return util::Result<void>::failure("session path is required");
    }
    if (must_exist && !std::filesystem::exists(path, ec)) {
        return util::Result<void>::failure("session file does not exist");
    }
    if (std::filesystem::exists(path, ec) && std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return util::Result<void>::failure("refusing to follow symlink session path");
    }
    return util::Result<void>::success();
}

util::Result<void> JsonlSessionStore::ensure_private_permissions(const std::filesystem::path& path, bool existing) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) {
        return util::Result<void>::failure("could not inspect session permissions: " + ec.message());
    }
    if (existing && has_public_read(status.permissions())) {
        return util::Result<void>::failure("session file is readable by group/others; refusing to load sensitive transcript");
    }
#if defined(__unix__) || defined(__APPLE__)
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return util::Result<void>::failure("could not set owner-only session permissions");
    }
#else
    (void)existing;
#endif
    return util::Result<void>::success();
}

boost::json::object JsonlSessionStore::header_to_json(const SessionMetadata& metadata) {
    boost::json::object object;
    object["type"] = "header";
    object["version"] = 1;
    object["session_id"] = metadata.session_id;
    object["created_at"] = metadata.created_at;
    object["workspace"] = metadata.workspace.string();
    object["provider"] = metadata.provider;
    object["model"] = metadata.model;
    return object;
}

util::Result<SessionMetadata> JsonlSessionStore::header_from_json(const boost::json::object& object) {
    SessionMetadata metadata;
    if (auto* v = object.if_contains("session_id"); v && v->is_string()) {
        metadata.session_id = std::string(v->as_string());
    }
    if (auto* v = object.if_contains("created_at"); v && v->is_string()) {
        metadata.created_at = std::string(v->as_string());
    }
    if (auto* v = object.if_contains("workspace"); v && v->is_string()) {
        metadata.workspace = std::string(v->as_string());
    }
    if (auto* v = object.if_contains("provider"); v && v->is_string()) {
        metadata.provider = std::string(v->as_string());
    }
    if (auto* v = object.if_contains("model"); v && v->is_string()) {
        metadata.model = std::string(v->as_string());
    }
    return util::Result<SessionMetadata>::success(std::move(metadata));
}

} // namespace cch::harness::session
