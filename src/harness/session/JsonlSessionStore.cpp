#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"

#include "../../../include/cch/ai/glaze/AiJson.hpp"
#include "../../../include/cch/util/Json.hpp"

#include "../../util/Redactor.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cch::harness::session {
namespace {

struct HeaderDto {
    std::string type{"header"};
    int version{2};
    std::string sessionId;
    std::string createdAt;
    std::string workspace;
    std::string provider;
    std::string model;
};

struct MessageEntryDto {
    std::string type{"message"};
    std::string entryId;
    ai::glaze::MessageDto message;
};

[[nodiscard]] util::Error session_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::Session, std::move(message), std::move(detail));
}

[[nodiscard]] util::Expected<std::string> entry_type(std::string_view line, std::size_t line_number) {
    auto parsed = util::read_json<util::JsonValue>(line);
    if (!parsed) {
        return std::unexpected(session_error(
            "malformed JSONL",
            "malformed JSONL at line " + std::to_string(line_number) + ": " + parsed.error().detail));
    }
    try {
        return parsed->get<util::JsonValue::object_t>().at("type").get_string();
    } catch (const std::exception&) {
        return std::unexpected(session_error(
            "session entry missing type",
            "session entry missing type at line " + std::to_string(line_number)));
    }
}

[[nodiscard]] HeaderDto to_dto(const SessionMetadata& metadata) {
    return HeaderDto{
        "header",
        2,
        metadata.session_id,
        metadata.created_at,
        metadata.workspace.string(),
        metadata.provider,
        metadata.model,
    };
}

[[nodiscard]] SessionMetadata from_dto(const HeaderDto& dto) {
    return SessionMetadata{dto.sessionId, dto.createdAt, dto.workspace, dto.provider, dto.model};
}

[[nodiscard]] MessageEntryDto to_dto(std::string entry_id, const ai::MessageVariant& message) {
    return MessageEntryDto{"message", std::move(entry_id), ai::glaze::to_message_dto(message)};
}

[[nodiscard]] util::JsonValue redact_json_value(const util::JsonValue& value) {
    if (const auto* text = value.get_if<std::string>()) {
        return util::JsonValue{util::redact_text(*text)};
    }
    if (const auto* values = value.get_if<util::JsonValue::array_t>()) {
        util::JsonValue::array_t redacted;
        redacted.reserve(values->size());
        for (const auto& item : *values) {
            redacted.push_back(redact_json_value(item));
        }
        return util::JsonValue{std::move(redacted)};
    }
    if (const auto* object = value.get_if<util::JsonValue::object_t>()) {
        util::JsonValue::object_t redacted;
        for (const auto& [key, item] : *object) {
            if (util::looks_secret_key(key)) {
                redacted.emplace(key, util::JsonValue{"[REDACTED]"});
            } else {
                redacted.emplace(key, redact_json_value(item));
            }
        }
        return util::JsonValue{std::move(redacted)};
    }
    return value;
}

void redact_content(ai::Content& content) {
    std::visit(
        [](auto& block) {
            using T = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<T, ai::TextContent>) {
                block.text = util::redact_text(std::move(block.text));
            } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
                block.thinking = util::redact_text(std::move(block.thinking));
            } else if constexpr (std::is_same_v<T, ai::ToolCallContent>) {
                if (block.arguments) {
                    block.arguments = redact_json_value(*block.arguments);
                }
                block.raw_arguments = util::redact_json_text(block.raw_arguments);
                if (block.argument_error) {
                    block.argument_error = util::redact_text(std::move(*block.argument_error));
                }
            }
        },
        content);
}

[[nodiscard]] ai::MessageVariant redacted_message(const ai::MessageVariant& message) {
    auto redacted = message;
    std::visit(
        [](auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, ai::SystemMessage>) {
                concrete.content = util::redact_text(std::move(concrete.content));
            } else {
                for (auto& block : concrete.content) {
                    redact_content(block);
                }
                if constexpr (std::is_same_v<T, ai::AssistantMessage>) {
                    if (concrete.error_message) {
                        concrete.error_message = util::redact_text(std::move(*concrete.error_message));
                    }
                } else if constexpr (std::is_same_v<T, ai::ToolResultMessage>) {
                    if (concrete.details) {
                        concrete.details = redact_json_value(*concrete.details);
                    }
                }
            }
        },
        redacted);
    return redacted;
}

bool has_public_read(std::filesystem::perms mode) {
    using std::filesystem::perms;
    return (mode & perms::others_read) != perms::none || (mode & perms::group_read) != perms::none;
}

util::ExpectedVoid write_new_file_exclusive(const std::filesystem::path& path, const std::string& content) {
#if defined(__unix__) || defined(__APPLE__)
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        return std::unexpected(session_error("could not create session file", std::strerror(errno)));
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
            return std::unexpected(session_error("could not write session header", message));
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return std::unexpected(session_error("could not flush session header", message));
    }
    if (::close(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return std::unexpected(session_error("could not close session file", message));
    }
    return {};
#else
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return std::unexpected(session_error("session file already exists", "use resume to append"));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(session_error("could not create session file"));
    }
    output << content;
    output.flush();
    output.close();
    if (!output) {
        return std::unexpected(session_error("could not write session header"));
    }
    return {};
#endif
}

} // namespace

util::Expected<JsonlSessionStore> JsonlSessionStore::create_new(const std::filesystem::path& path, SessionMetadata metadata) {
    auto validation = validate_session_path_for_open(path, false);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(session_error("could not create session directory", ec.message()));
        }
    }
    if (std::filesystem::exists(path, ec)) {
        return std::unexpected(session_error("session file already exists", "use --resume to append"));
    }
    auto header_json = util::write_json(to_dto(metadata));
    if (!header_json) {
        return std::unexpected(header_json.error());
    }
    auto header = write_new_file_exclusive(path, *header_json + '\n');
    if (!header) {
        return std::unexpected(header.error());
    }
    if (auto perms = ensure_private_permissions(path, false); !perms) {
        return std::unexpected(perms.error());
    }
    JsonlSessionStore store;
    store.path_ = path;
    store.metadata_ = std::move(metadata);
    return store;
}

util::Expected<JsonlSessionStore> JsonlSessionStore::open_existing(const std::filesystem::path& path) {
    auto loaded = load(path);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    JsonlSessionStore store;
    store.path_ = path;
    store.metadata_ = loaded->metadata;
    store.next_entry_id_ = loaded->messages.size() + 1;
    return store;
}

util::Expected<LoadedSession> JsonlSessionStore::load(const std::filesystem::path& path) {
    auto validation = validate_session_path_for_open(path, true);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    if (auto perms = ensure_private_permissions(path, true); !perms) {
        return std::unexpected(perms.error());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(session_error("could not open session file"));
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
        auto type = entry_type(line, line_number);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (line_number == 1 && *type == "header") {
            auto header = util::read_json<HeaderDto>(line);
            if (!header) {
                return std::unexpected(header.error());
            }
            loaded.metadata = from_dto(*header);
            SessionEntry entry;
            entry.kind = SessionEntryKind::Header;
            entry.raw_line = line;
            loaded.entries.push_back(std::move(entry));
            saw_header = true;
            continue;
        }
        if (*type == "message") {
            auto dto = util::read_json<MessageEntryDto>(line);
            if (!dto) {
                return std::unexpected(dto.error());
            }
            auto message = ai::glaze::message_from_dto(dto->message, line);
            if (!message) {
                return std::unexpected(message.error());
            }
            SessionEntry entry;
            entry.kind = SessionEntryKind::Message;
            entry.entry_id = dto->entryId;
            entry.message = *message;
            entry.raw_line = line;
            loaded.messages.push_back(*message);
            loaded.entries.push_back(std::move(entry));
        } else {
            SessionEntry entry;
            entry.kind = SessionEntryKind::Unknown;
            entry.raw_line = line;
            loaded.entries.push_back(std::move(entry));
            loaded.unknown_lines.push_back(line);
        }
    }
    if (input.bad()) {
        return std::unexpected(session_error("could not read complete session file"));
    }
    if (!saw_header) {
        return std::unexpected(session_error("session header is missing"));
    }
    return loaded;
}

util::ExpectedVoid JsonlSessionStore::append(const ai::MessageVariant& message) {
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) {
        return std::unexpected(session_error("could not append to session file"));
    }
    auto redacted = redacted_message(message);
    auto entry_json = util::write_json(to_dto("m" + std::to_string(next_entry_id_), redacted));
    if (!entry_json) {
        return std::unexpected(entry_json.error());
    }
    output << *entry_json << '\n';
    output.flush();
    output.close();
    if (!output) {
        return std::unexpected(session_error("could not persist session entry"));
    }
    ++next_entry_id_;
    return {};
}

util::ExpectedVoid JsonlSessionStore::validate_session_path_for_open(const std::filesystem::path& path, bool must_exist) {
    std::error_code ec;
    if (path.empty()) {
        return std::unexpected(session_error("session path is required"));
    }
    if (must_exist && !std::filesystem::exists(path, ec)) {
        return std::unexpected(session_error("session file does not exist"));
    }
    if (std::filesystem::exists(path, ec) && std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
        return std::unexpected(session_error("refusing to follow symlink session path"));
    }
    return {};
}

util::ExpectedVoid JsonlSessionStore::ensure_private_permissions(const std::filesystem::path& path, bool existing) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) {
        return std::unexpected(session_error("could not inspect session permissions", ec.message()));
    }
    if (existing && has_public_read(status.permissions())) {
        return std::unexpected(session_error("session file is readable by group/others", "refusing to load sensitive transcript"));
    }
#if defined(__unix__) || defined(__APPLE__)
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(session_error("could not set owner-only session permissions"));
    }
#else
    (void)existing;
#endif
    return {};
}

} // namespace cch::harness::session
