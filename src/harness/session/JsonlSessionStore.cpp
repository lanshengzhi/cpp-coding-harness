#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"

#include "../../../include/cch/ai/glaze/AiJson.hpp"
#include "../../../include/cch/util/Json.hpp"

#include "../../util/Redactor.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
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
    std::string type{"session"};
    int version{3};
    std::string id;
    std::string timestamp;
    std::string cwd;
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

// --- v3 tree entry DTOs ---
// Each mirrors pi's session-format.md field names exactly.
// Common fields (type, id, parentId, timestamp) are inline — no base struct
// because Glaze reflection operates on the concrete type.

struct ModelChangeDto {
    std::string type{"model_change"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string provider;
    std::string modelId;
};

struct ThinkingLevelChangeDto {
    std::string type{"thinking_level_change"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string thinkingLevel;
};

struct ActiveToolsChangeDto {
    std::string type{"active_tools_change"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::vector<std::string> tools;
};

struct CustomDto {
    std::string type{"custom"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string customType;
    glz::raw_json data{"null"};
};

struct CustomMessageDto {
    std::string type{"custom_message"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string customType;
    std::string content;
    bool display{true};
    std::optional<glz::raw_json> details;
};

struct LabelDto {
    std::string type{"label"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string targetId;
    std::optional<std::string> label;
};

struct CompactionDto {
    std::string type{"compaction"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string summary;
    std::string firstKeptEntryId;
    std::size_t tokensBefore{0};
    std::optional<glz::raw_json> details;
    std::optional<bool> fromHook;
};

struct BranchSummaryDto {
    std::string type{"branch_summary"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string fromId;
    std::string summary;
    std::optional<glz::raw_json> details;
    std::optional<bool> fromHook;
};

struct SessionInfoDto {
    std::string type{"session_info"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string name;
};

[[nodiscard]] util::Error session_error(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::Session, std::move(message), std::move(detail));
}

[[nodiscard]] util::Expected<std::string> entry_type(const glz::generic& parsed, std::size_t line_number) {
    try {
        return parsed.get<glz::generic::object_t>().at("type").get<std::string>();
    } catch (const std::exception&) {
        return std::unexpected(session_error(
            "session entry missing type",
            "session entry missing type at line " + std::to_string(line_number)));
    }
}

template <typename T>
[[nodiscard]] util::Expected<T> entry_from_generic(const glz::generic& value, std::string_view line, std::size_t line_number) {
    auto parsed = glz::read_json<T>(value);
    if (!parsed) {
        return std::unexpected(session_error(
            "failed to parse session entry",
            "failed to parse session entry at line " + std::to_string(line_number) + ": " +
                glz::format_error(parsed.error(), line)));
    }
    return std::move(parsed).value();
}

[[nodiscard]] std::string generate_entry_id() {
    thread_local std::random_device rd;
    thread_local std::mt19937_64 gen(rd());
    thread_local std::uniform_int_distribution<unsigned> dist(0, 15);
    const char hex_chars[] = "0123456789abcdef";
    std::string id(8, '0');
    for (auto& c : id) {
        c = hex_chars[dist(gen)];
    }
    return id;
}

[[nodiscard]] std::string generate_iso_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm gm{};
    gmtime_r(&time, &gm);
    std::ostringstream oss;
    oss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] util::ExpectedVoid append_line_to_file(const std::filesystem::path& path, const std::string& line) {
    int flags = O_WRONLY | O_APPEND | O_CREAT;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
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
    return {};
}
#else
[[nodiscard]] util::ExpectedVoid append_line_to_file(const std::filesystem::path& path, const std::string& line) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        return std::unexpected(session_error("could not append to session file"));
    }
    output << line;
    output.flush();
    output.close();
    if (!output) {
        return std::unexpected(session_error("could not persist session entry"));
    }
    return {};
}
#endif

// Serialize a DTO and append to file. Tree entry metadata does not contain
// user secrets — key-based redaction is too aggressive for fields like
// "tokensBefore" (which contains "token"). Per-type methods handle any
// payload redaction (e.g., custom.data) before constructing the DTO.
template <typename Dto>
[[nodiscard]] util::ExpectedVoid write_entry_line(const std::filesystem::path& path, const Dto& dto, std::size_t& counter) {
    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    const auto line = *json_str + '\n';
    auto result = append_line_to_file(path, line);
    if (result) {
        ++counter;
    }
    return result;
}

[[nodiscard]] WriteHeaderDto to_dto(const SessionMetadata& metadata) {
    return WriteHeaderDto{
        "session",
        3,
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
            }
        },
        content);
}

void redact_assistant_content(ai::AssistantContent& content) {
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
            } else if constexpr (std::is_same_v<T, ai::AssistantMessage>) {
                for (auto& block : concrete.content) {
                    redact_assistant_content(block);
                }
                if (concrete.error_message) {
                    concrete.error_message = util::redact_text(std::move(*concrete.error_message));
                }
            } else if constexpr (std::is_same_v<T, ai::BashExecutionMessage>) {
                concrete.command = util::redact_text(std::move(concrete.command));
                concrete.output = util::redact_text(std::move(concrete.output));
            } else if constexpr (std::is_same_v<T, ai::CustomMessage>) {
                for (auto& block : concrete.content) {
                    redact_content(block);
                }
                if (concrete.details) {
                    concrete.details = redact_json_value(*concrete.details);
                }
            } else if constexpr (std::is_same_v<T, ai::BranchSummaryMessage>) {
                // Summary text is already plain; no further redaction needed
            } else if constexpr (std::is_same_v<T, ai::CompactionSummaryMessage>) {
                // Summary text is already plain; no further redaction needed
            } else {
                for (auto& block : concrete.content) {
                    redact_content(block);
                }
                if constexpr (std::is_same_v<T, ai::ToolResultMessage>) {
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
    JsonlSessionStore store;
    store.path_ = path;
    store.metadata_ = loaded->metadata;
    store.next_entry_id_ = loaded->entries.size();
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
        auto generic = glz::read_json<glz::generic>(line);
        if (!generic) {
            return std::unexpected(session_error(
                "malformed JSONL",
                "malformed JSONL at line " + std::to_string(line_number) + ": " +
                    glz::format_error(generic.error(), line)));
        }
        auto type = entry_type(*generic, line_number);
        if (!type) {
            return std::unexpected(type.error());
        }
        auto payload = util::json_from_glaze(*generic);
        if (line_number == 1 && (*type == "header" || *type == "session")) {
            auto header = entry_from_generic<ReadHeaderDto>(*generic, line, line_number);
            if (!header) {
                return std::unexpected(header.error());
            }
            loaded.metadata = from_dto(*header);
            SessionEntry entry;
            entry.kind = SessionEntryKind::Header;
            entry.raw_line = line;
            entry.payload = std::move(payload);
            populate_tree_fields(entry, entry.payload);
            loaded.entries.push_back(std::move(entry));
            saw_header = true;
            continue;
        }
        auto kind = kind_from_type(*type);
        if (kind == SessionEntryKind::Message) {
            auto dto = entry_from_generic<MessageEntryDto>(*generic, line, line_number);
            if (!dto) {
                return std::unexpected(dto.error());
            }
            auto message = ai::glaze::message_from_dto(dto->message, line);
            if (!message) {
                return std::unexpected(message.error());
            }
            SessionEntry entry;
            entry.kind = SessionEntryKind::Message;
            entry.entry_id = !dto->entryId.empty() ? dto->entryId : dto->id;
            entry.parent_id = dto->parentId;
            entry.leaf_id = dto->leafId;
            entry.message = *message;
            entry.payload = std::move(payload);
            entry.raw_line = line;
            loaded.messages.push_back(*message);
            loaded.entries.push_back(std::move(entry));
        } else {
            SessionEntry entry;
            entry.kind = kind;
            entry.raw_line = line;
            entry.payload = std::move(payload);
            populate_tree_fields(entry, entry.payload);
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
    auto entry_json = util::write_json(to_dto(generate_entry_id(), redacted));
    if (!entry_json) {
        return std::unexpected(entry_json.error());
    }
    auto result = append_line_to_file(path_, *entry_json + '\n');
    if (result) {
        ++next_entry_id_;
    }
    return result;
}

// --- v3 tree entry append methods ---

util::ExpectedVoid JsonlSessionStore::append_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) {
    ModelChangeDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.provider = std::move(provider);
    dto.modelId = std::move(model_id);
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) {
    ThinkingLevelChangeDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.thinkingLevel = std::move(thinking_level);
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_active_tools_change(
    std::optional<std::string> parent_id,
    std::vector<std::string> tools) {
    ActiveToolsChangeDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.tools = std::move(tools);
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    util::JsonValue data) {
    auto data_json = util::write_json(data);
    if (!data_json) {
        return std::unexpected(session_error("failed to serialize custom entry data"));
    }
    CustomDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.customType = std::move(custom_type);
    dto.data = glz::raw_json{std::move(*data_json)};
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_custom_message_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    std::string content,
    bool display,
    std::optional<util::JsonValue> details) {
    CustomMessageDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.customType = std::move(custom_type);
    dto.content = std::move(content);
    dto.display = display;
    if (details) {
        auto details_json = util::write_json(*details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize custom message details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) {
    LabelDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.targetId = std::move(target_id);
    dto.label = std::move(label);
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_compaction(
    std::optional<std::string> parent_id,
    std::string summary,
    std::string first_kept_entry_id,
    std::size_t tokens_before,
    std::optional<util::JsonValue> details,
    std::optional<bool> from_hook) {
    CompactionDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.summary = std::move(summary);
    dto.firstKeptEntryId = std::move(first_kept_entry_id);
    dto.tokensBefore = tokens_before;
    if (details) {
        auto details_json = util::write_json(*details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize compaction details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }
    dto.fromHook = from_hook;
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<util::JsonValue> details,
    std::optional<bool> from_hook) {
    BranchSummaryDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.fromId = std::move(from_id);
    dto.summary = std::move(summary);
    if (details) {
        auto details_json = util::write_json(*details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize branch summary details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }
    dto.fromHook = from_hook;
    return write_entry_line(path_, dto, next_entry_id_);
}

util::ExpectedVoid JsonlSessionStore::append_session_info(
    std::optional<std::string> parent_id,
    std::string name) {
    SessionInfoDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.name = std::move(name);
    return write_entry_line(path_, dto, next_entry_id_);
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
