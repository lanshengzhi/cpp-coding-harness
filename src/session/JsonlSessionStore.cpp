#include "JsonlSessionStore.hpp"

#include "../util/Redactor.hpp"

#include <boost/json.hpp>

#include <iomanip>
#include <sstream>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace cch::session {
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

} // namespace

util::Result<JsonlSessionStore> JsonlSessionStore::create_new(const std::filesystem::path& path, SessionMetadata metadata) {
    auto validation = validate_session_path_for_open(path, false);
    if (!validation) {
        return util::Result<JsonlSessionStore>::failure(validation.error());
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return util::Result<JsonlSessionStore>::failure("could not create session directory: " + ec.message());
    }
    if (std::filesystem::exists(path, ec)) {
        return util::Result<JsonlSessionStore>::failure("session file already exists; use resume to append");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return util::Result<JsonlSessionStore>::failure("could not create session file");
    }
    output << boost::json::serialize(header_to_json(metadata)) << '\n';
    output.close();
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
            loaded.messages.push_back(message.value());
        } else {
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
    agent::Message redacted = message;
    redacted.content = util::redact_text(redacted.content);
    boost::json::object entry;
    entry["type"] = "message";
    entry["entry_id"] = "m" + std::to_string(next_entry_id_++);
    entry["message"] = agent::message_to_json(redacted);
    output << boost::json::serialize(entry) << '\n';
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

} // namespace cch::session
