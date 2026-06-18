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

struct WriteHeaderDto {
    std::string type{"header"};
    int version{2};
    std::string sessionId;
    std::string createdAt;
    std::string workspace;
    std::string provider;
    std::string model;
};

struct ReadHeaderDto {
    std::string type{"header"};
    int version{2};
    std::optional<std::string> sessionId;
    std::optional<std::string> createdAt;
    std::optional<std::string> workspace;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> id;
    std::optional<std::string> timestamp;
    std::optional<std::string> cwd;
};

struct MessageEntryDto {
    std::string type{"message"};
    std::string entryId;
    std::string id;
    std::optional<std::string> parentId;
    std::optional<std::string> leafId;
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

[[nodiscard]] WriteHeaderDto to_dto(const SessionMetadata& metadata) {
    return WriteHeaderDto{
        "header",
        2,
        metadata.session_id,
        metadata.created_at,
        metadata.workspace.string(),
        metadata.provider,
        metadata.model,
    };
}

[[nodiscard]] SessionMetadata from_dto(const ReadHeaderDto& dto) {
    if (dto.type == "session") {
        return SessionMetadata{
            dto.id.value_or({}),
            dto.timestamp.value_or({}),
            dto.cwd.value_or({}),
            dto.provider.value_or({}),
            dto.model.value_or({})};
    }
    return SessionMetadata{
        dto.sessionId.value_or({}),
        dto.createdAt.value_or({}),
        dto.workspace.value_or({}),
        dto.provider.value_or({}),
        dto.model.value_or({})};
}

[[nodiscard]] MessageEntryDto to_dto(std::string entry_id, const ai::MessageVariant& message) {
    MessageEntryDto dto;
    dto.type = "message";
    dto.entryId = std::move(entry_id);
    dto.message = ai::glaze::to_message_dto(message);
    return dto;
}

[[nodiscard]] SessionEntryKind kind_from_type(const std::string& type) {
    if (type == "header" || type == "session") return SessionEntryKind::Header;
    if (type == "message") return SessionEntryKind::Message;
    if (type == "model_change") return SessionEntryKind::ModelChange;
    if (type == "thinking_level_change") return SessionEntryKind::ThinkingLevelChange;
    if (type == "active_tools_change") return SessionEntryKind::ActiveToolsChange;
    if (type == "custom") return SessionEntryKind::Custom;
    if (type == "custom_message") return SessionEntryKind::CustomMessage;
    if (type == "label") return SessionEntryKind::Label;
    if (type == "compaction") return SessionEntryKind::Compaction;
    if (type == "branch_summary") return SessionEntryKind::BranchSummary;
    if (type == "session_info") return SessionEntryKind::SessionInfo;
    if (type == "leaf") return SessionEntryKind::Leaf;
    return SessionEntryKind::Unknown;
}

[[nodiscard]] std::optional<std::string> optional_string_field(
    const util::JsonValue::object_t& object,
    const std::string& key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (const auto* value = found->second.get_if<std::string>()) {
        return *value;
    }
    return std::nullopt;
}

void populate_tree_fields(SessionEntry& entry, const util::JsonValue& value) {
    if (const auto* object = value.get_if<util::JsonValue::object_t>()) {
        if (auto id = optional_string_field(*object, "entryId")) {
            entry.entry_id = *id;
        } else if (auto id = optional_string_field(*object, "id")) {
            entry.entry_id = *id;
        }
        entry.parent_id = optional_string_field(*object, "parentId");
        entry.leaf_id = optional_string_field(*object, "leafId");
    }
}

[[nodiscard]] bool parse_only_tree_kind(SessionEntryKind kind) {
    switch (kind) {
    case SessionEntryKind::ModelChange:
    case SessionEntryKind::ThinkingLevelChange:
    case SessionEntryKind::ActiveToolsChange:
    case SessionEntryKind::Custom:
    case SessionEntryKind::CustomMessage:
    case SessionEntryKind::Label:
    case SessionEntryKind::Compaction:
    case SessionEntryKind::BranchSummary:
    case SessionEntryKind::SessionInfo:
    case SessionEntryKind::Leaf:
        return true;
    case SessionEntryKind::Header:
    case SessionEntryKind::Message:
    case SessionEntryKind::Unknown:
        return false;
    }
    return false;
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

[[maybe_unused]] bool has_public_read(std::filesystem::perms mode) {
    using std::filesystem::perms;
    return (mode & perms::others_read) != perms::none || (mode & perms::group_read) != perms::none;
}

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] int open_session_path(const std::filesystem::path& path, int flags, int mode = 0) {
    int final_flags = flags;
#ifdef O_NOFOLLOW
    final_flags |= O_NOFOLLOW;
#endif
    if (mode != 0) {
        return ::open(path.c_str(), final_flags, mode);
    }
    return ::open(path.c_str(), final_flags);
}

[[nodiscard]] util::Expected<std::string> read_file_contents(int fd) {
    std::string contents;
    std::array<char, 8192> buffer{};
    for (;;) {
        ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            return std::unexpected(session_error("could not read session file", std::strerror(errno)));
        }
        if (n == 0) {
            break;
        }
        contents.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return contents;
}

[[nodiscard]] util::ExpectedVoid set_fd_private_permissions(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return std::unexpected(session_error("could not inspect session permissions", std::strerror(errno)));
    }
    if ((st.st_mode & (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) != 0) {
        return std::unexpected(session_error("session file is readable by group/others", "refusing to load sensitive transcript"));
    }
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        return std::unexpected(session_error("could not set owner-only session permissions", std::strerror(errno)));
    }
    return {};
}
#endif

[[nodiscard]] bool parent_path_contains_symlink(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::path cursor;
    for (const auto& part : parent) {
        if (part == "/" || part == "." || part.empty()) {
            continue;
        }
        if (part == "..") {
            // Should already be normalized by caller; treat as suspicious.
            return true;
        }
        cursor /= part;
        auto status = std::filesystem::symlink_status(cursor, ec);
        if (ec) {
            return status.type() != std::filesystem::file_type::not_found;
        }
        if (std::filesystem::is_symlink(status)) {
            return true;
        }
    }
    return false;
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
        if (parent_path_contains_symlink(path)) {
            return std::unexpected(session_error("session parent path contains a symlink", "refusing to create session directory through a symlink"));
        }
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
    for (const auto& entry : loaded->entries) {
        if (parse_only_tree_kind(entry.kind)) {
            return std::unexpected(session_error(
                "session tree entries require tree resume support",
                "refusing to append to a session containing parse-only tree entries until tree reconstruction is implemented"));
        }
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
#if defined(__unix__) || defined(__APPLE__)
    int fd = open_session_path(path, O_RDONLY);
    if (fd == -1) {
        return std::unexpected(session_error("could not open session file", std::strerror(errno)));
    }
    auto fd_guard = std::unique_ptr<int, void (*)(int*)>(new int(fd), [](int* p) { if (p && *p != -1) ::close(*p); delete p; });
    if (auto perms = set_fd_private_permissions(fd); !perms) {
        return std::unexpected(perms.error());
    }
    auto contents = read_file_contents(fd);
    if (!contents) {
        return std::unexpected(contents.error());
    }
#else
    if (auto perms = ensure_private_permissions(path, true); !perms) {
        return std::unexpected(perms.error());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(session_error("could not open session file"));
    }
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
#endif

    LoadedSession loaded;
    std::string line;
    std::size_t line_number = 0;
    bool saw_header = false;
    std::istringstream stream(*contents);
    while (std::getline(stream, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        auto type = entry_type(line, line_number);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (line_number == 1 && (*type == "header" || *type == "session")) {
            auto header = util::read_json<ReadHeaderDto>(line);
            if (!header) {
                return std::unexpected(header.error());
            }
            loaded.metadata = from_dto(*header);
            auto parsed = util::read_json<util::JsonValue>(line);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            SessionEntry entry;
            entry.kind = SessionEntryKind::Header;
            entry.raw_line = line;
            entry.payload = *parsed;
            populate_tree_fields(entry, *parsed);
            loaded.entries.push_back(std::move(entry));
            saw_header = true;
            continue;
        }
        auto kind = kind_from_type(*type);
        if (kind == SessionEntryKind::Message) {
            auto dto = util::read_json<MessageEntryDto>(line);
            if (!dto) {
                return std::unexpected(dto.error());
            }
            auto message = ai::glaze::message_from_dto(dto->message, line);
            if (!message) {
                return std::unexpected(message.error());
            }
            auto parsed = util::read_json<util::JsonValue>(line);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            SessionEntry entry;
            entry.kind = SessionEntryKind::Message;
            entry.entry_id = !dto->entryId.empty() ? dto->entryId : dto->id;
            entry.parent_id = dto->parentId;
            entry.leaf_id = dto->leafId;
            entry.message = *message;
            entry.payload = *parsed;
            entry.raw_line = line;
            loaded.messages.push_back(*message);
            loaded.entries.push_back(std::move(entry));
        } else {
            auto parsed = util::read_json<util::JsonValue>(line);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            SessionEntry entry;
            entry.kind = kind;
            entry.raw_line = line;
            entry.payload = *parsed;
            populate_tree_fields(entry, *parsed);
            loaded.entries.push_back(std::move(entry));
            if (kind == SessionEntryKind::Unknown) {
                loaded.unknown_lines.push_back(line);
            }
        }
    }
#if defined(__unix__) || defined(__APPLE__)
    // read_file_contents returns full content; stream parsing errors are caught above.
#else
    if (input.bad()) {
        return std::unexpected(session_error("could not read complete session file"));
    }
#endif
    if (!saw_header) {
        return std::unexpected(session_error("session header is missing"));
    }
    return loaded;
}

util::ExpectedVoid JsonlSessionStore::append(const ai::MessageVariant& message) {
    auto redacted = redacted_message(message);
    auto entry_json = util::write_json(to_dto("m" + std::to_string(next_entry_id_), redacted));
    if (!entry_json) {
        return std::unexpected(entry_json.error());
    }
    const auto line = *entry_json + '\n';

#if defined(__unix__) || defined(__APPLE__)
    int flags = O_WRONLY | O_APPEND | O_CREAT;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path_.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        return std::unexpected(session_error("could not append to session file", std::strerror(errno)));
    }
    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            const auto message = std::string(std::strerror(errno));
            ::close(fd);
            return std::unexpected(session_error("could not write session entry", message));
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const auto message = std::string(std::strerror(errno));
        ::close(fd);
        return std::unexpected(session_error("could not persist session entry", message));
    }
    if (::close(fd) != 0) {
        return std::unexpected(session_error("could not close session file", std::strerror(errno)));
    }
#else
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) {
        return std::unexpected(session_error("could not append to session file"));
    }
    output << line;
    output.flush();
    output.close();
    if (!output) {
        return std::unexpected(session_error("could not persist session entry"));
    }
#endif
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
#if defined(__unix__) || defined(__APPLE__)
    int fd = open_session_path(path, O_RDONLY);
    if (fd == -1) {
        return std::unexpected(session_error("could not open session file for permission check", std::strerror(errno)));
    }
    auto fd_guard = std::unique_ptr<int, void (*)(int*)>(new int(fd), [](int* p) { if (p && *p != -1) ::close(*p); delete p; });
    if (auto perms = set_fd_private_permissions(fd); !perms) {
        if (!existing) {
            // When creating a new file, permission errors are still failures but the message above assumes "existing".
            auto detail = perms.error().detail.empty()
                ? "could not set owner-only session permissions"
                : perms.error().detail;
            return std::unexpected(session_error("could not set owner-only session permissions", detail));
        }
        return std::unexpected(perms.error());
    }
    return {};
#else
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) {
        return std::unexpected(session_error("could not inspect session permissions", ec.message()));
    }
    if (existing && has_public_read(status.permissions())) {
        return std::unexpected(session_error("session file is readable by group/others", "refusing to load sensitive transcript"));
    }
    (void)existing;
    return {};
#endif
}

} // namespace cch::harness::session
