#include <cch/durable/JsonlStorage.hpp>

#include <cch/chord/Delta.hpp>
#include <cch/chord/Json.hpp>
#include <cch/support/JsonAccess.hpp>

#include "support/UniqueFd.hpp"

#include "StorageEncoding.hpp"
#include "StorageState.hpp"
#include "durable/JsonlIoTestHooks.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace cch::durable {

namespace {

std::mutex jsonl_io_hook_mutex;
testing::JsonlIoHook jsonl_io_hook;

} // namespace

namespace testing {

void set_jsonl_io_hook(JsonlIoHook hook) {
    std::lock_guard lock(jsonl_io_hook_mutex);
    jsonl_io_hook = std::move(hook);
}

void clear_jsonl_io_hook() {
    std::lock_guard lock(jsonl_io_hook_mutex);
    jsonl_io_hook = nullptr;
}

bool fail_jsonl_io(const JsonlIoEvent& event) {
    JsonlIoHook hook;
    {
        std::lock_guard lock(jsonl_io_hook_mutex);
        hook = jsonl_io_hook;
    }
    return hook && hook(event);
}

} // namespace testing

namespace {

constexpr double kFormatVersion = 1.0;
constexpr std::string_view kMainFile = "main.jsonl";
constexpr std::string_view kReclaimSuffix = ".reclaim";
constexpr std::string_view kPoisonMessage = "JSONL storage is poisoned and must be reopened";
constexpr std::string_view kClosedMessage = "JSONL storage is closed";

[[nodiscard]] support::Error corruption(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

[[nodiscard]] testing::JsonlIoEvent io_event(
        testing::JsonlIoOperation operation, std::string_view file, std::string_view destination = {}) {
    return testing::JsonlIoEvent{
            .operation = operation, .file = std::string{file}, .destination = std::string{destination}};
}

[[nodiscard]] bool is_format(const support::JsonValue& value) {
    const auto* number = value.get_if<double>();
    return number != nullptr && *number == kFormatVersion;
}

[[nodiscard]] support::JsonValue object(
        std::initializer_list<std::pair<const std::string, support::JsonValue>> members) {
    support::JsonValue::object_t result;
    for (const auto& [key, value] : members) {
        result.emplace(key, value);
    }
    return support::JsonValue{std::move(result)};
}

[[nodiscard]] support::JsonValue entry_value(const EntryRecord& entry) {
    support::JsonValue::object_t value;
    value.emplace("id", number(entry.id.value));
    value.emplace("conversationId", number(entry.conversation_id.value));
    value.emplace("kind", support::JsonValue{entry.kind});
    value.emplace("data", entry.data.to_support());
    if (entry.head.has_value()) {
        value.emplace("head", number(entry.head->value));
    }
    if (!entry.model.empty()) {
        support::JsonValue::array_t model;
        for (const auto& message : entry.model) {
            model.push_back(encode_json(message));
        }
        value.emplace("model", support::JsonValue{std::move(model)});
    }
    if (entry.by_task_id.has_value()) {
        value.emplace("byTaskId", number(entry.by_task_id->value));
    }
    if (!entry.edits.empty()) {
        support::JsonValue::array_t edits;
        for (const ContextEdit& edit : entry.edits) {
            support::JsonValue::array_t messages;
            for (const chord::Json& message : edit.messages)
                messages.push_back(encode_json(message));
            edits.push_back(object({{"target", number(edit.target.value)},
                    {"action", support::JsonValue{edit.action}},
                    {"messages", support::JsonValue{std::move(messages)}}}));
        }
        value.emplace("edits", support::JsonValue{std::move(edits)});
    }
    return support::JsonValue{std::move(value)};
}

[[nodiscard]] support::JsonValue document_scope_value(const DocumentRecord& record) {
    switch (record.scope()) {
    case DocumentScope::Session:
        return object({{"kind", support::JsonValue{"session"}}});
    case DocumentScope::Conversation:
        return object({{"kind", support::JsonValue{"conversation"}},
                {"conversationId", number(record.conversation_id->value)}});
    case DocumentScope::Task:
        return object({{"kind", support::JsonValue{"task"}}, {"taskId", number(record.task_id->value)}});
    }
    return object({{"kind", support::JsonValue{"session"}}});
}

[[nodiscard]] std::string_view history_name(DocumentHistory history) noexcept {
    return history == DocumentHistory::Rewindable ? "rewindable" : "latest";
}

[[nodiscard]] std::string_view fork_name(DocumentFork fork) noexcept {
    switch (fork) {
    case DocumentFork::AsOf:
        return "asOf";
    case DocumentFork::Current:
        return "current";
    case DocumentFork::Initial:
        return "initial";
    }
    return "current";
}

[[nodiscard]] support::JsonValue document_create_record(const DocumentRecord& record) {
    support::JsonValue::object_t value;
    value.emplace("id", number(record.id.value));
    value.emplace("kind", support::JsonValue{record.kind});
    if (record.key.has_value()) {
        value.emplace("key", support::JsonValue{*record.key});
    }
    value.emplace("scope", document_scope_value(record));
    if (record.scope() == DocumentScope::Conversation) {
        value.emplace("history", support::JsonValue{std::string{history_name(record.history)}});
        value.emplace("fork", support::JsonValue{std::string{fork_name(record.fork)}});
    }
    return support::JsonValue{std::move(value)};
}

[[nodiscard]] std::string document_file(DocumentId id) { return "doc-" + std::to_string(id.value) + ".jsonl"; }

[[nodiscard]] std::optional<DocumentId> document_id_from_file(std::string_view name) {
    if (!name.starts_with("doc-") || !name.ends_with(".jsonl")) {
        return std::nullopt;
    }
    const std::string_view digits = name.substr(4, name.size() - 4 - 6);
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || value < 0) {
        return std::nullopt;
    }
    return DocumentId{.value = value};
}

[[nodiscard]] std::optional<TaskId> task_id_from_file(std::string_view name) {
    if (!name.starts_with("task-")) return std::nullopt;
    const auto id = document_id_from_file(std::string{"doc-"} + std::string{name.substr(5)});
    if (!id) return std::nullopt;
    return TaskId{.value = id->value};
}

[[nodiscard]] std::string task_file(TaskId id) { return "task-" + std::to_string(id.value) + ".jsonl"; }

[[nodiscard]] bool is_reclaim_file(std::string_view name) {
    if (!name.ends_with(kReclaimSuffix)) return false;
    const auto base = name.substr(0, name.size() - kReclaimSuffix.size());
    return document_id_from_file(base).has_value() || task_id_from_file(base).has_value();
}

[[nodiscard]] std::string sidecar_key(std::string_view file, Seq seq, std::int64_t ordinal) {
    std::string key{file};
    key.push_back('\n');
    key += std::to_string(seq);
    key.push_back('\n');
    key += std::to_string(ordinal);
    return key;
}

[[nodiscard]] bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80U) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if ((byte & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = byte & 0x1FU;
        } else if ((byte & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = byte & 0x0FU;
        } else if ((byte & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = byte & 0x07U;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        const std::uint32_t minimum = length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U;
        if (code_point < minimum || code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += length;
    }
    return true;
}

struct FileIdentity {
    dev_t device{};
    ino_t inode{};

    [[nodiscard]] bool operator==(const FileIdentity&) const = default;
};

struct OpenedTaskFile {
    support::UniqueFd fd;
    FileIdentity identity{};
};

/// Open a task sidecar relative to the captured store directory. O_NOFOLLOW
/// rejects a final symlink, and the regular single-link check rejects hard
/// links before anything is read, truncated, or appended.
[[nodiscard]] support::Expected<OpenedTaskFile> open_task_file(int directory_fd, std::string_view name, int flags) {
    const std::string entry{name};
    const int fd = ::openat(directory_fd, entry.c_str(), flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return std::unexpected(corruption("JSONL task sidecar open failed"));
    support::UniqueFd file{fd};
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        return std::unexpected(corruption("JSONL task sidecar is not a singly linked regular file"));
    return OpenedTaskFile{.fd = std::move(file), .identity = FileIdentity{.device = info.st_dev, .inode = info.st_ino}};
}

/// Unlink only the task sidecar whose identity was observed while opening the
/// store. A substituted entry is left alone; a link placed between this check
/// and unlinkat remains a race outside the single-owner storage contract.
[[nodiscard]] support::ExpectedVoid reclaim_task_file(
        int directory_fd, std::string_view name, std::map<std::string, FileIdentity>& identities) {
    const std::string entry{name};
    const auto known = identities.find(entry);
    if (known == identities.end()) return {};
    struct stat info{};
    if (::fstatat(directory_fd, entry.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            identities.erase(entry);
            return {};
        }
        return std::unexpected(corruption("JSONL sidecar reclamation failed"));
    }
    if (!S_ISREG(info.st_mode) || info.st_dev != known->second.device || info.st_ino != known->second.inode) {
        // Keep the observed identity on a mismatch: the substituted entry must
        // keep failing a later append instead of being adopted.
        return {};
    }
    if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Remove, entry))) {
        return std::unexpected(corruption("JSONL sidecar reclamation failed"));
    }
    if (::unlinkat(directory_fd, entry.c_str(), 0) != 0 && errno != ENOENT) {
        return std::unexpected(corruption("JSONL sidecar reclamation failed"));
    }
    // A future commit for this task creates a new inode. Drop the observed
    // identity once the entry is gone, otherwise that commit fails closed
    // against an identity no longer present on disk.
    identities.erase(entry);
    return {};
}

struct RawLine {
    std::string text;
    std::size_t start{0};
};

[[nodiscard]] std::size_t complete_prefix_size(std::string_view bytes) {
    if (bytes.empty() || bytes.back() == '\n') return bytes.size();
    const std::size_t last_newline = bytes.rfind('\n');
    return last_newline == std::string_view::npos ? 0 : last_newline + 1;
}

[[nodiscard]] support::Expected<off_t> checked_offset(std::size_t value) {
    if (!std::in_range<off_t>(value)) {
        return std::unexpected(corruption("JSONL truncation offset is out of range"));
    }
    return static_cast<off_t>(value);
}

[[nodiscard]] support::Expected<std::vector<RawLine>> split_complete_lines(
        std::string_view bytes, std::string_view name) {
    std::vector<RawLine> lines;
    std::size_t start = 0;
    std::size_t number = 1;
    while (start < bytes.size()) {
        const std::size_t end = bytes.find('\n', start);
        if (end == std::string_view::npos) {
            break;
        }
        std::string text{bytes.substr(start, end - start)};
        if (!valid_utf8(text)) {
            return std::unexpected(
                    corruption("Invalid UTF-8 in complete " + std::string{name} + " line " + std::to_string(number)));
        }
        lines.push_back(RawLine{.text = std::move(text), .start = start});
        start = end + 1;
        ++number;
    }
    return lines;
}

[[nodiscard]] support::Expected<std::vector<RawLine>> read_complete_lines(
        const std::filesystem::path& path, std::string_view name) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(corruption("JSONL read of " + std::string{name} + " failed"));
    }
    if (!exists) {
        return std::vector<RawLine>{};
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::unexpected(corruption("JSONL read of " + std::string{name} + " failed"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(corruption("JSONL read of " + std::string{name} + " failed"));
    }
    std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (stream.bad()) {
        return std::unexpected(corruption("JSONL read of " + std::string{name} + " failed"));
    }

    const std::size_t complete_size = complete_prefix_size(bytes);
    if (complete_size != bytes.size()) {
        std::filesystem::resize_file(path, complete_size, error);
        if (error) {
            return std::unexpected(corruption("JSONL torn-line truncation of " + std::string{name} + " failed"));
        }
        bytes.resize(complete_size);
    }
    return split_complete_lines(bytes, name);
}

[[nodiscard]] support::Expected<std::vector<RawLine>> read_complete_lines(
        const support::UniqueFd& file, std::string_view name) {
    std::string bytes;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return std::unexpected(corruption("JSONL read of " + std::string{name} + " failed"));
        if (count == 0) break;
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }

    const std::size_t complete_size = complete_prefix_size(bytes);
    if (complete_size != bytes.size()) {
        auto offset = checked_offset(complete_size);
        if (!offset) {
            return std::unexpected(std::move(offset.error()));
        }
        if (::ftruncate(file.get(), *offset) != 0) {
            return std::unexpected(corruption("JSONL torn-line truncation of " + std::string{name} + " failed"));
        }
        bytes.resize(complete_size);
    }
    return split_complete_lines(bytes, name);
}

[[nodiscard]] support::ExpectedVoid append_file(const std::filesystem::path& path, std::string_view content) {
    if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Append, path.filename().string()))) {
        return std::unexpected(corruption("JSONL append failed"));
    }
    std::error_code error;
    // A missing target is the normal first-append case; only an existing
    // directory (the injected I/O failure) must not be opened as a file.
    if (std::filesystem::is_directory(path, error)) {
        return std::unexpected(corruption("JSONL append failed"));
    }
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        return std::unexpected(corruption("JSONL append failed"));
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream.good()) {
        return std::unexpected(corruption("JSONL append failed"));
    }
    return {};
}

/// Flush one open file (frozen pi `FileSystem.flushFile`). The flush is a
/// separate physical operation from the append, so it can fail on its own.
[[nodiscard]] support::ExpectedVoid flush_descriptor(int fd, std::string_view name) {
    if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Flush, name))) {
        return std::unexpected(corruption("JSONL flush failed"));
    }
    while (::fsync(fd) != 0) {
        if (errno == EINTR) continue;
        return std::unexpected(corruption("JSONL flush failed"));
    }
    return {};
}

/// Flush one store file by path: frozen pi reopens the path read-write and
/// `fsync`s it, so a missing or replaced path fails the flush as well.
[[nodiscard]] support::ExpectedVoid flush_file(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    support::UniqueFd file{::open(path.c_str(), O_RDWR | O_CLOEXEC)};
    if (!file) {
        return std::unexpected(corruption("JSONL flush failed"));
    }
    return flush_descriptor(file.get(), name);
}

/// Best-effort removal of one published sidecar (frozen pi `remove`).
void remove_file(const std::filesystem::path& path) {
    if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Remove, path.filename().string()))) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}

[[nodiscard]] support::ExpectedVoid replace_file(
        const std::filesystem::path& directory, std::string_view name, std::string_view content, bool sync) {
    const std::string temporary_name = std::string{name} + std::string{kReclaimSuffix};
    const std::filesystem::path temporary = directory / temporary_name;
    if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Write, temporary_name))) {
        return std::unexpected(corruption("JSONL sidecar reclamation failed"));
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return std::unexpected(corruption("JSONL sidecar reclamation failed"));
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream.good()) {
            return std::unexpected(corruption("JSONL sidecar reclamation failed"));
        }
    }
    if (sync) {
        if (auto flushed = flush_file(temporary); !flushed) {
            // The marker already published this state; the scratch file stays
            // for recovery to remove and the replacement is retried on reopen.
            return std::unexpected(corruption("JSONL sidecar reclamation failed"));
        }
    }
    if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Rename, temporary_name, name))) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return std::unexpected(corruption("JSONL sidecar reclamation failed"));
    }
    std::error_code error;
    std::filesystem::rename(temporary, directory / name, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(corruption("JSONL sidecar reclamation failed"));
    }
    return {};
}

[[nodiscard]] support::Expected<chord::Json> decode_json(const support::JsonValue& value) {
    return chord::Json::from_support(value);
}

struct DocumentContent {
    bool base{true};
    std::uint32_t version{1};
    chord::Json value{};
    chord::OpBatch ops{};
};

struct SidecarPayload {
    DocumentId id{};
    DocumentContent content{};
    std::optional<TaskRecord> task{};
};

struct SidecarRecord {
    Seq seq{0};
    std::int64_t ordinal{0};
    SidecarPayload payload{};
};

struct ParsedSidecar {
    SidecarRecord record;
    std::size_t start{0};
};

enum class MainKind {
    Conversation,
    Entry,
    Task,
    TaskSidecar,
    Submission,
    DocumentCreate,
    DocumentCopy,
    DocumentChange,
    DocumentRetire
};

struct MainOperation {
    MainKind kind{MainKind::Conversation};
    ConversationRecord conversation{};
    EntryRecord entry{};
    TaskRecord task{};
    TaskId task_id{};
    SubmissionRecord submission{};
    DocumentRecord document_record{};
    DocumentCopySource document_copy_source{};
    DocumentId document_id{};
    std::int64_t ordinal{0};
};

struct MainMarker {
    Seq seq{0};
    std::vector<MainOperation> writes{};
};

[[nodiscard]] support::Expected<DocumentContent> parse_content(const support::JsonValue& value) {
    const auto* content = value.get_if<support::JsonValue::object_t>();
    if (content == nullptr) {
        return std::unexpected(corruption("Invalid document content"));
    }
    const auto version = support::json_integer_member(*content, "version");
    const auto kind = support::json_string_member(*content, "kind");
    if (!version || *version < 1 || !kind) {
        return std::unexpected(corruption("Invalid document content"));
    }
    if (*kind == "base") {
        const auto* base = support::json_member(*content, "value");
        if (base == nullptr) {
            return std::unexpected(corruption("Invalid document content"));
        }
        auto decoded = decode_json(*base);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        return DocumentContent{
                .base = true, .version = static_cast<std::uint32_t>(*version), .value = std::move(*decoded), .ops = {}};
    }
    if (*kind == "delta") {
        const auto* ops = support::json_member(*content, "ops");
        if (ops == nullptr) {
            return std::unexpected(corruption("Invalid document content"));
        }
        auto decoded = decode_ops(*ops);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        return DocumentContent{.base = false,
                .version = static_cast<std::uint32_t>(*version),
                .value = {},
                .ops = std::move(*decoded)};
    }
    return std::unexpected(corruption("Invalid document content"));
}

[[nodiscard]] support::Expected<SidecarRecord> parse_sidecar(
        std::string_view text, std::string_view file, std::size_t line) {
    auto parsed = support::read_json(text);
    if (!parsed) {
        return std::unexpected(corruption("Malformed complete " + std::string{file} + " line " + std::to_string(line)));
    }
    const auto* root = parsed->get_if<support::JsonValue::object_t>();
    const auto* format = root == nullptr ? nullptr : support::json_member(*root, "format");
    if (root == nullptr || format == nullptr || !is_format(*format) ||
            support::json_string_member(*root, "type") != std::optional<std::string_view>{"record"}) {
        return std::unexpected(
                corruption("Invalid sidecar record in " + std::string{file} + " line " + std::to_string(line)));
    }
    const auto seq = support::json_integer_member(*root, "seq");
    const auto ordinal = support::json_integer_member(*root, "ordinal");
    const auto* payload = support::json_object_member(*root, "payload");
    if (!seq || *seq < 1 || !ordinal || *ordinal < 0 || !payload) {
        return std::unexpected(
                corruption("Invalid sidecar record in " + std::string{file} + " line " + std::to_string(line)));
    }
    const auto type = support::json_string_member(*payload, "type");
    if (type == std::optional<std::string_view>{"task"}) {
        const auto* value = support::json_member(*payload, "value");
        if (value == nullptr) return std::unexpected(corruption("Invalid live task record in " + std::string{file}));
        auto json = chord::Json::from_support(*value);
        if (!json) return std::unexpected(std::move(json.error()));
        auto task = decode_task_record(*json);
        if (!task || task->state.status == "terminal")
            return std::unexpected(corruption("Invalid live task record in " + std::string{file}));
        return SidecarRecord{.seq = *seq, .ordinal = *ordinal, .payload = SidecarPayload{.task = std::move(*task)}};
    }
    if (type != std::optional<std::string_view>{"document"})
        return std::unexpected(corruption("Invalid sidecar record in " + std::string{file}));
    const auto id = support::json_integer_member(*payload, "id");
    const auto* content = support::json_member(*payload, "content");
    if (!id || content == nullptr) {
        return std::unexpected(
                corruption("Invalid document record in " + std::string{file} + " line " + std::to_string(line)));
    }
    auto decoded_content = parse_content(*content);
    if (!decoded_content) {
        return std::unexpected(std::move(decoded_content.error()));
    }
    return SidecarRecord{.seq = *seq,
            .ordinal = *ordinal,
            .payload = SidecarPayload{.id = DocumentId{.value = *id}, .content = std::move(*decoded_content)}};
}

[[nodiscard]] support::Expected<EntryRecord> parse_entry(
        const support::JsonValue& value, std::string_view description) {
    const auto* entry = value.get_if<support::JsonValue::object_t>();
    if (entry == nullptr) {
        return std::unexpected(corruption("Invalid entry write in " + std::string{description}));
    }
    const auto id = support::json_integer_member(*entry, "id");
    const auto conversation_id = support::json_integer_member(*entry, "conversationId");
    const auto kind = support::json_string_member(*entry, "kind");
    const auto* data = support::json_member(*entry, "data");
    if (!id || !conversation_id || !kind) {
        return std::unexpected(corruption("Invalid entry write in " + std::string{description}));
    }
    chord::Json decoded_data{};
    if (data != nullptr) {
        auto decoded = decode_json(*data);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        decoded_data = std::move(*decoded);
    }
    std::optional<EntryId> head;
    if (const auto* raw_head = support::json_member(*entry, "head");
            raw_head != nullptr && !raw_head->holds<support::JsonValue::null_t>()) {
        const auto parsed_head = nonnegative_integer(*raw_head);
        if (!parsed_head) {
            return std::unexpected(corruption("Invalid entry write in " + std::string{description}));
        }
        head = EntryId{.value = *parsed_head};
    }
    std::vector<chord::Json> model;
    if (const auto* messages = support::json_array_member(*entry, "model")) {
        model.reserve(messages->size());
        for (const auto& message : *messages) {
            auto decoded = decode_json(message);
            if (!decoded) {
                return std::unexpected(corruption("Invalid entry write in " + std::string{description}));
            }
            model.push_back(std::move(*decoded));
        }
    }
    std::optional<TaskId> by_task_id;
    if (const auto raw_task_id = support::json_integer_member(*entry, "byTaskId")) {
        by_task_id = TaskId{.value = *raw_task_id};
    }
    std::vector<ContextEdit> edits;
    if (const auto* raw_edits = support::json_array_member(*entry, "edits")) {
        for (const support::JsonValue& raw_edit : *raw_edits) {
            const auto* object = raw_edit.get_if<support::JsonValue::object_t>();
            if (object == nullptr)
                return std::unexpected(corruption("Invalid context edit in " + std::string{description}));
            const auto target = support::json_integer_member(*object, "target");
            const auto action = support::json_string_member(*object, "action");
            const auto* raw_messages = support::json_array_member(*object, "messages");
            if (!target || !action || raw_messages == nullptr) {
                return std::unexpected(corruption("Invalid context edit in " + std::string{description}));
            }
            ContextEdit edit{.target = EntryId{.value = *target}, .action = std::string{*action}};
            for (const support::JsonValue& raw_message : *raw_messages) {
                auto decoded = decode_json(raw_message);
                if (!decoded) return std::unexpected(corruption("Invalid context edit in " + std::string{description}));
                edit.messages.push_back(std::move(*decoded));
            }
            edits.push_back(std::move(edit));
        }
    }
    return EntryRecord{.id = EntryId{.value = *id},
            .conversation_id = ConversationId{.value = *conversation_id},
            .kind = std::string{*kind},
            .data = std::move(decoded_data),
            .head = head,
            .model = std::move(model),
            .edits = std::move(edits),
            .by_task_id = by_task_id};
}

[[nodiscard]] support::Expected<DocumentRecord> parse_document_record(const support::JsonValue& value) {
    const auto* record = value.get_if<support::JsonValue::object_t>();
    if (record == nullptr) {
        return std::unexpected(corruption("Invalid document record"));
    }
    const auto id = support::json_integer_member(*record, "id");
    const auto kind = support::json_string_member(*record, "kind");
    const auto* scope = support::json_object_member(*record, "scope");
    const auto scope_kind = scope == nullptr ? std::nullopt : support::json_string_member(*scope, "kind");
    if (!id || !kind || !scope_kind) {
        return std::unexpected(corruption("Invalid document record"));
    }
    DocumentRecord parsed{.id = DocumentId{.value = *id}, .kind = std::string{*kind}};
    if (const auto key = support::json_string_member(*record, "key")) {
        parsed.key = std::string{*key};
    }
    if (*scope_kind == "conversation") {
        const auto conversation_id = support::json_integer_member(*scope, "conversationId");
        if (!conversation_id) {
            return std::unexpected(corruption("Invalid document record"));
        }
        parsed.conversation_id = ConversationId{.value = *conversation_id};
        const auto history = support::json_string_member(*record, "history");
        const auto fork = support::json_string_member(*record, "fork");
        if (history == std::optional<std::string_view>{"rewindable"}) {
            parsed.history = DocumentHistory::Rewindable;
        } else if (history != std::optional<std::string_view>{"latest"}) {
            return std::unexpected(corruption("Invalid document record"));
        }
        if (fork == std::optional<std::string_view>{"asOf"}) {
            parsed.fork = DocumentFork::AsOf;
        } else if (fork == std::optional<std::string_view>{"initial"}) {
            parsed.fork = DocumentFork::Initial;
        } else if (fork != std::optional<std::string_view>{"current"}) {
            return std::unexpected(corruption("Invalid document record"));
        }
    } else if (*scope_kind == "task") {
        const auto task_id = support::json_integer_member(*scope, "taskId");
        if (!task_id) {
            return std::unexpected(corruption("Invalid document record"));
        }
        parsed.task_id = TaskId{.value = *task_id};
    } else if (*scope_kind != "session") {
        return std::unexpected(corruption("Invalid document record"));
    }
    if (const auto retired_at = support::json_integer_member(*record, "retiredAt")) {
        parsed.retired_at = *retired_at;
    }
    return parsed;
}

[[nodiscard]] support::Expected<DocumentCopySource> parse_document_copy_source(const support::JsonValue& value) {
    const auto* source = value.get_if<support::JsonValue::object_t>();
    if (source == nullptr) {
        return std::unexpected(corruption("Invalid document copy source"));
    }
    const auto id = support::json_integer_member(*source, "id");
    if (!id) {
        return std::unexpected(corruption("Invalid document copy source"));
    }
    DocumentCopySource parsed{.id = DocumentId{.value = *id}};
    const auto* at = support::json_member(*source, "at");
    if (at == nullptr) {
        return std::unexpected(corruption("Invalid document copy source"));
    }
    if (const auto* seq = at->get_if<double>()) {
        parsed.at = DocumentPoint::as_of(static_cast<Seq>(*seq));
    } else if (const auto* name = at->get_if<std::string>(); name == nullptr || *name != "current") {
        return std::unexpected(corruption("Invalid document copy source"));
    }
    return parsed;
}

[[nodiscard]] support::Expected<MainOperation> parse_main_operation(
        const support::JsonValue& value, std::string_view description) {
    const auto* operation = value.get_if<support::JsonValue::object_t>();
    if (operation == nullptr) {
        return std::unexpected(corruption("Invalid write in " + std::string{description}));
    }
    const auto type = support::json_string_member(*operation, "type");
    if (!type) {
        return std::unexpected(corruption("Invalid write in " + std::string{description}));
    }
    if (*type == "conversation") {
        const auto* record = support::json_member(*operation, "value");
        if (record == nullptr) {
            return std::unexpected(corruption("Invalid conversation write in " + std::string{description}));
        }
        auto json = chord::Json::from_support(*record);
        if (!json) {
            return std::unexpected(std::move(json.error()));
        }
        auto decoded = decode_conversation_record(*json);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        return MainOperation{.kind = MainKind::Conversation, .conversation = std::move(*decoded)};
    }
    if (*type == "entry") {
        const auto* record = support::json_member(*operation, "value");
        if (record == nullptr) {
            return std::unexpected(corruption("Invalid entry write in " + std::string{description}));
        }
        auto entry = parse_entry(*record, description);
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return MainOperation{.kind = MainKind::Entry, .entry = std::move(*entry)};
    }
    if (*type == "task") {
        const auto* record = support::json_member(*operation, "value");
        if (record == nullptr) {
            return std::unexpected(corruption("Invalid task write in " + std::string{description}));
        }
        auto json = chord::Json::from_support(*record);
        if (!json) {
            return std::unexpected(std::move(json.error()));
        }
        auto decoded = decode_task_record(*json);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        return MainOperation{.kind = MainKind::Task, .task = std::move(*decoded)};
    }
    if (*type == "task.sidecar") {
        const auto id = support::json_integer_member(*operation, "id");
        const auto ordinal = support::json_integer_member(*operation, "ordinal");
        if (!id || *id < 0 || !ordinal || *ordinal < 0)
            return std::unexpected(corruption("Invalid task sidecar write in " + std::string{description}));
        return MainOperation{.kind = MainKind::TaskSidecar, .task_id = TaskId{.value = *id}, .ordinal = *ordinal};
    }
    if (*type == "submission") {
        const auto* record = support::json_member(*operation, "value");
        if (record == nullptr) {
            return std::unexpected(corruption("Invalid submission write in " + std::string{description}));
        }
        auto json = chord::Json::from_support(*record);
        if (!json) {
            return std::unexpected(std::move(json.error()));
        }
        auto decoded = decode_submission_record(*json);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        return MainOperation{.kind = MainKind::Submission, .submission = std::move(*decoded)};
    }
    if (*type == "document.retire") {
        const auto id = support::json_integer_member(*operation, "id");
        if (!id) {
            return std::unexpected(corruption("Invalid document retirement in " + std::string{description}));
        }
        return MainOperation{.kind = MainKind::DocumentRetire, .document_id = DocumentId{.value = *id}};
    }
    if (*type == "document.create") {
        const auto* record = support::json_object_member(*operation, "record");
        const auto id = record == nullptr ? std::nullopt : support::json_integer_member(*record, "id");
        const auto kind = record == nullptr ? std::nullopt : support::json_string_member(*record, "kind");
        const auto ordinal = support::json_integer_member(*operation, "ordinal");
        if (!id || !kind || !ordinal || *ordinal < 0) {
            return std::unexpected(corruption("Invalid document creation in " + std::string{description}));
        }
        auto parsed_record = parse_document_record(*record);
        if (!parsed_record) {
            return std::unexpected(std::move(parsed_record.error()));
        }
        parsed_record->id = DocumentId{.value = *id};
        parsed_record->kind = std::string{*kind};
        return MainOperation{
                .kind = MainKind::DocumentCreate, .document_record = std::move(*parsed_record), .ordinal = *ordinal};
    }
    if (*type == "document.copy") {
        const auto* record = support::json_object_member(*operation, "record");
        if (record == nullptr)
            return std::unexpected(corruption("Invalid document copy in " + std::string{description}));
        const auto id = support::json_integer_member(*record, "id");
        const auto ordinal = support::json_integer_member(*operation, "ordinal");
        const auto* source = support::json_object_member(*operation, "source");
        if (!id || !ordinal || *ordinal < 0 || source == nullptr || record == nullptr) {
            return std::unexpected(corruption("Invalid document copy in " + std::string{description}));
        }
        auto parsed_record = parse_document_record(*record);
        if (!parsed_record) {
            return std::unexpected(std::move(parsed_record.error()));
        }
        parsed_record->id = DocumentId{.value = *id};
        auto parsed_source = parse_document_copy_source(*source);
        if (!parsed_source) {
            return std::unexpected(std::move(parsed_source.error()));
        }
        return MainOperation{.kind = MainKind::DocumentCopy,
                .document_record = std::move(*parsed_record),
                .document_copy_source = std::move(*parsed_source),
                .ordinal = *ordinal};
    }
    if (*type == "document.change") {
        const auto id = support::json_integer_member(*operation, "id");
        const auto ordinal = support::json_integer_member(*operation, "ordinal");
        if (!id || !ordinal || *ordinal < 0) {
            return std::unexpected(corruption("Invalid document change in " + std::string{description}));
        }
        return MainOperation{
                .kind = MainKind::DocumentChange, .document_id = DocumentId{.value = *id}, .ordinal = *ordinal};
    }
    return std::unexpected(corruption("Unknown write type in " + std::string{description}));
}

[[nodiscard]] support::Expected<MainMarker> parse_main_marker(std::string_view text, std::size_t line) {
    auto parsed = support::read_json(text);
    const std::string description = std::string{kMainFile} + " line " + std::to_string(line);
    if (!parsed) {
        return std::unexpected(corruption("Malformed complete " + description));
    }
    const auto* marker = parsed->get_if<support::JsonValue::object_t>();
    if (marker == nullptr) {
        return std::unexpected(corruption("Invalid commit marker in " + description));
    }
    const auto* format = support::json_member(*marker, "format");
    const auto seq = support::json_integer_member(*marker, "seq");
    const auto* writes = support::json_array_member(*marker, "writes");
    if (format == nullptr || !is_format(*format) ||
            support::json_string_member(*marker, "type") != std::optional<std::string_view>{"commit"} || !seq ||
            *seq < 1 || writes == nullptr) {
        return std::unexpected(corruption("Invalid commit marker in " + description));
    }
    MainMarker result{.seq = *seq, .writes = {}};
    result.writes.reserve(writes->size());
    for (const support::JsonValue& write : *writes) {
        auto operation = parse_main_operation(write, description);
        if (!operation) {
            return std::unexpected(std::move(operation.error()));
        }
        result.writes.push_back(std::move(*operation));
    }
    return result;
}

[[nodiscard]] std::string sidecar_line(const SidecarRecord& record) {
    if (record.payload.task) {
        auto text = support::write_json(object({{"format", support::JsonValue{1}},
                {"type", support::JsonValue{"record"}},
                {"seq", number(record.seq)},
                {"ordinal", number(record.ordinal)},
                {"payload",
                        object({{"type", support::JsonValue{"task"}},
                                {"value", encode_task_record(*record.payload.task).to_support()}})}}));
        return text ? *text : std::string{};
    }
    const support::JsonValue value = object({
            {"format", support::JsonValue{1}},
            {"type", support::JsonValue{"record"}},
            {"seq", number(record.seq)},
            {"ordinal", number(record.ordinal)},
            {"payload",
                    object({
                            {"type", support::JsonValue{"document"}},
                            {"id", number(record.payload.id.value)},
                            {"content",
                                    object({
                                            {"version", support::JsonValue{1}},
                                            {"kind",
                                                    support::JsonValue{record.payload.content.base ? "base" : "delta"}},
                                            {record.payload.content.base ? "value" : "ops",
                                                    record.payload.content.base
                                                            ? record.payload.content.value.to_support()
                                                            : ops_value(record.payload.content.ops)},
                                    })},
                    })},
    });
    auto text = support::write_json(value);
    return text ? *text : std::string{};
}

struct EncodedCommit {
    std::string marker;
    std::map<std::string, std::string> sidecars;
    std::map<std::string, std::string> reclamations;
};

/// Read and parse one existing document sidecar file.
[[nodiscard]] support::Expected<std::vector<SidecarRecord>> load_sidecar_records(
        const std::filesystem::path& directory, std::string_view file) {
    auto lines = read_complete_lines(directory / file, file);
    if (!lines) return std::unexpected(std::move(lines.error()));
    std::vector<SidecarRecord> records;
    records.reserve(lines->size());
    for (std::size_t index = 0; index < lines->size(); ++index) {
        auto record = parse_sidecar((*lines)[index].text, file, index + 1);
        if (!record) return std::unexpected(std::move(record.error()));
        records.push_back(std::move(*record));
    }
    return records;
}

[[nodiscard]] EncodedCommit encode_commit(Seq seq,
        std::span<const StorageWrite> writes,
        const std::set<std::int64_t>& current_only_documents,
        const std::filesystem::path& directory) {
    std::vector<support::JsonValue> main_writes;
    std::map<std::string, std::string> sidecars;
    std::set<std::int64_t> retired;
    std::map<std::string, std::size_t> base_offsets;
    std::int64_t next_ordinal = 0;

    const auto add_sidecar = [&](std::string file, SidecarRecord record) {
        record.ordinal = next_ordinal++;
        sidecars[file] += sidecar_line(record) + "\n";
        return record.ordinal;
    };

    for (const StorageWrite& write : writes) {
        if (write.type == "conversation") {
            main_writes.push_back(object({{"type", support::JsonValue{"conversation"}},
                    {"value", encode_json(encode_conversation_record(*write.conversation))}}));
        } else if (write.type == "entry") {
            main_writes.push_back(
                    object({{"type", support::JsonValue{"entry"}}, {"value", entry_value(*write.entry)}}));
        } else if (write.type == "task") {
            if (write.task.has_value() && write.task->state.status == "terminal") {
                main_writes.push_back(object({
                        {"type", support::JsonValue{"task"}},
                        {"value", encode_task_record(*write.task).to_support()},
                }));
            } else if (write.task.has_value()) {
                const auto ordinal = add_sidecar(task_file(write.task->id),
                        SidecarRecord{.seq = seq, .payload = SidecarPayload{.task = *write.task}});
                main_writes.push_back(object({{"type", support::JsonValue{"task.sidecar"}},
                        {"id", number(write.task->id.value)},
                        {"ordinal", number(ordinal)}}));
            }
        } else if (write.type == "submission") {
            if (write.submission.has_value()) {
                main_writes.push_back(object({
                        {"type", support::JsonValue{"submission"}},
                        {"value", encode_submission_record(*write.submission).to_support()},
                }));
            }
        } else if (write.type == "document.create") {
            const std::string file = document_file(write.document_record->id);
            const std::int64_t ordinal = add_sidecar(file,
                    SidecarRecord{.seq = seq,
                            .ordinal = 0,
                            .payload = SidecarPayload{.id = write.document_record->id,
                                    .content = DocumentContent{.base = true,
                                            .version = write.document_version,
                                            .value = *write.document_value,
                                            .ops = {}}}});
            main_writes.push_back(object({{"type", support::JsonValue{"document.create"}},
                    {"record", document_create_record(*write.document_record)},
                    {"ordinal", number(ordinal)}}));
        } else if (write.type == "document.copy") {
            // A copy persists the source's materialized base as the new
            // incarnation's first revision.
            const DocumentCopySource& source = *write.document_copy_source;
            const std::string source_file = document_file(source.id);
            auto source_sidecars = load_sidecar_records(directory, source_file);
            if (!source_sidecars) {
                return {};
            }
            const bool source_current_only = document_is_current_only(write.document_record.value());
            std::vector<const SidecarRecord*> source_revisions;
            source_revisions.reserve(source_sidecars->size());
            for (const SidecarRecord& revision : *source_sidecars) {
                if (source.at.at.has_value() && revision.seq > *source.at.at) break;
                source_revisions.push_back(&revision);
            }
            if (source.at.at.has_value() && source_current_only) {
                return {};
            }
            std::size_t base_index = source_revisions.size();
            while (base_index > 0 && !source_revisions[base_index - 1]->payload.content.base) {
                --base_index;
            }
            if (base_index == 0) {
                return {};
            }
            const DocumentContent& base = source_revisions[base_index - 1]->payload.content;
            chord::Json materialized = base.value;
            for (std::size_t index = base_index; index < source_revisions.size(); ++index) {
                auto applied =
                        chord::apply_immutable_batches(materialized, {source_revisions[index]->payload.content.ops});
                if (!applied) return {};
                materialized = std::move(*applied);
            }
            const std::string file = document_file(write.document_record->id);
            const std::int64_t ordinal = add_sidecar(file,
                    SidecarRecord{.seq = seq,
                            .ordinal = 0,
                            .payload = SidecarPayload{.id = write.document_record->id,
                                    .content = DocumentContent{.base = true,
                                            .version = base.version,
                                            .value = std::move(materialized),
                                            .ops = {}}}});
            main_writes.push_back(object({{"type", support::JsonValue{"document.copy"}},
                    {"record", document_create_record(*write.document_record)},
                    {"source",
                            object({{"id", number(write.document_copy_source->id.value)},
                                    {"at",
                                            write.document_copy_source->at.at.has_value()
                                                    ? number(*write.document_copy_source->at.at)
                                                    : support::JsonValue{"current"}}})},
                    {"ordinal", number(ordinal)}}));
        } else if (write.type == "document.change") {
            const std::string file = document_file(*write.document_id);
            const bool base = write.document_ops.empty();
            const std::size_t base_offset = sidecars[file].size();
            const std::int64_t ordinal = add_sidecar(file,
                    SidecarRecord{.seq = seq,
                            .ordinal = 0,
                            .payload = SidecarPayload{.id = *write.document_id,
                                    .content = DocumentContent{.base = base,
                                            .version = write.document_version,
                                            .value = write.document_value.value_or(chord::Json{}),
                                            .ops = write.document_ops}}});
            if (base) {
                base_offsets[file] = base_offset;
            }
            main_writes.push_back(object({{"type", support::JsonValue{"document.change"}},
                    {"id", number(write.document_id->value)},
                    {"ordinal", number(ordinal)}}));
        } else if (write.type == "document.retire") {
            retired.insert(write.document_id->value);
            main_writes.push_back(object(
                    {{"type", support::JsonValue{"document.retire"}}, {"id", number(write.document_id->value)}}));
        }
    }

    support::JsonValue::array_t encoded_writes;
    encoded_writes.reserve(main_writes.size());
    for (support::JsonValue& write : main_writes) {
        encoded_writes.push_back(std::move(write));
    }
    const support::JsonValue marker = object({{"format", support::JsonValue{1}},
            {"type", support::JsonValue{"commit"}},
            {"seq", number(seq)},
            {"writes", support::JsonValue{std::move(encoded_writes)}}});
    auto marker_text = support::write_json(marker);
    EncodedCommit encoded{.marker = marker_text ? *marker_text + "\n" : std::string{},
            .sidecars = std::move(sidecars),
            .reclamations = {}};
    // Terminal publication makes a live task sidecar reclaimable. Reclaiming
    // only after the marker leaves a published result recoverable on failure.
    std::map<std::int64_t, bool> final_tasks;
    for (const StorageWrite& write : writes) {
        if (write.type == "task" && write.task)
            final_tasks[write.task->id.value] = write.task->state.status == "terminal";
    }
    for (const auto& [id, terminal] : final_tasks) {
        if (terminal) encoded.reclamations[task_file(TaskId{.value = id})] = {};
    }
    // A current-only incarnation keeps no history, so its sidecar is removed
    // on retirement and replaced by its newest base. A rewindable incarnation
    // keeps its sidecar so as-of reads still reach earlier revisions.
    for (const std::int64_t id : retired) {
        if (current_only_documents.contains(id)) {
            encoded.reclamations[document_file(DocumentId{.value = id})] = {};
        }
    }
    for (const auto& [file, offset] : base_offsets) {
        const auto id = document_id_from_file(file);
        if (!id || retired.contains(id->value) || !current_only_documents.contains(id->value)) {
            continue;
        }
        encoded.reclamations[file] = encoded.sidecars[file].substr(offset);
    }
    return encoded;
}

} // namespace

struct JsonlStorage::Impl {
    explicit Impl(std::filesystem::path directory, JsonlStorageOptions options)
        : directory(std::move(directory)), options(options) {}

    std::filesystem::path directory;
    JsonlStorageOptions options;
    // Captured once so task sidecar access and reclamation stay in the opened
    // store even if a later path lookup would resolve elsewhere.
    support::UniqueFd directory_fd;
    // Identity of every task sidecar observed in this incarnation. Appends and
    // reclamation reject a substituted entry instead of writing to or deleting
    // a file the store did not create or adopt during recovery.
    std::map<std::string, FileIdentity> task_identities;
    StorageState state{std::string{kClosedMessage}};
    bool poisoned{false};

    [[nodiscard]] support::Error poison_error() const {
        return support::make_error(support::ErrorCode::Validation, std::string{kPoisonMessage});
    }

    [[nodiscard]] std::optional<support::Error> unusable() const {
        if (poisoned) {
            return poison_error();
        }
        if (state.closed()) {
            return support::make_error(support::ErrorCode::Session, std::string{kClosedMessage});
        }
        return std::nullopt;
    }

    /// Flush one sidecar the store just appended. A task sidecar is reopened
    /// through the no-follow, single-link boundary and must still be the entry
    /// observed at append time, so a substituted file is never flushed as if
    /// the store had written it (#1085).
    [[nodiscard]] support::ExpectedVoid flush_sidecar(std::string_view file) {
        if (!task_id_from_file(file)) {
            return flush_file(directory / std::string{file});
        }
        auto opened = open_task_file(directory_fd.get(), file, O_RDWR);
        if (!opened) return std::unexpected(std::move(opened.error()));
        const auto known = task_identities.find(std::string{file});
        if (known == task_identities.end() || !(known->second == opened->identity)) {
            return std::unexpected(corruption("JSONL task sidecar changed before its flush"));
        }
        return flush_descriptor(opened->fd.get(), file);
    }

    /// Apply one reclamation pass (frozen pi `reclaimSidecars`). The marker
    /// already published this state, so a failure defers the whole pass and
    /// never changes a commit or recovery outcome. With fsync, `main.jsonl` is
    /// flushed first: its marker bytes authorize every replacement and removal.
    void reclaim_sidecars(const std::map<std::string, std::string>& replacements) {
        if (replacements.empty()) {
            return;
        }
        if (options.fsync) {
            // Flushing main authorizes the replacement before a sidecar is
            // removed or rewritten.
            if (auto flushed = flush_file(directory / kMainFile); !flushed) {
                return;
            }
        }
        for (const auto& [file, content] : replacements) {
            if (task_id_from_file(file)) {
                // Task sidecars are reclaimed whole, relative to the captured
                // directory, and only while the observed identity still holds.
                if (content.empty()) {
                    (void)reclaim_task_file(directory_fd.get(), file, task_identities);
                }
                continue;
            }
            if (content.empty()) {
                remove_file(directory / file);
            } else {
                (void)replace_file(directory, file, content, options.fsync);
            }
        }
    }

    [[nodiscard]] support::ExpectedVoid recover() {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error || !std::filesystem::is_directory(directory, error) || error) {
            return std::unexpected(corruption("JSONL directory creation failed"));
        }
        const int directory_handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_handle < 0) {
            return std::unexpected(corruption("JSONL directory open failed"));
        }
        directory_fd = support::UniqueFd{directory_handle};

        auto main_lines = read_complete_lines(directory / kMainFile, kMainFile);
        if (!main_lines) {
            return std::unexpected(std::move(main_lines.error()));
        }
        std::vector<MainMarker> markers;
        markers.reserve(main_lines->size());
        Seq previous_seq = 0;
        for (std::size_t index = 0; index < main_lines->size(); ++index) {
            auto marker = parse_main_marker((*main_lines)[index].text, index + 1);
            if (!marker) {
                return std::unexpected(std::move(marker.error()));
            }
            if (marker->seq <= previous_seq) {
                return std::unexpected(corruption("Commit sequence does not strictly increase in main.jsonl"));
            }
            previous_seq = marker->seq;
            markers.push_back(std::move(*marker));
        }

        std::vector<std::string> sidecar_files;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) {
            return std::unexpected(corruption("JSONL directory listing failed"));
        }
        for (const std::filesystem::directory_entry& entry : iterator) {
            const std::string name = entry.path().filename().string();
            if (task_id_from_file(name).has_value()) {
                sidecar_files.push_back(name);
                continue;
            }
            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error) || entry_error) {
                continue;
            }
            if (is_reclaim_file(name)) {
                remove_file(entry.path());
                continue;
            }
            if (document_id_from_file(name).has_value() || task_id_from_file(name).has_value()) {
                sidecar_files.push_back(name);
            }
        }
        std::sort(sidecar_files.begin(), sidecar_files.end());

        std::map<std::string, std::vector<ParsedSidecar>> parsed_files;
        std::map<std::string, FileIdentity> task_identities;
        std::map<std::string, const SidecarRecord*> record_by_key;
        for (const std::string& file : sidecar_files) {
            // One sidecar is open at a time: recovery holds no descriptor after
            // its bytes are read, so the sidecar count never consumes the fd budget.
            std::vector<RawLine> lines;
            if (task_id_from_file(file)) {
                auto opened = open_task_file(directory_fd.get(), file, O_RDWR);
                if (!opened) return std::unexpected(std::move(opened.error()));
                task_identities.emplace(file, opened->identity);
                auto read = read_complete_lines(opened->fd, file);
                if (!read) return std::unexpected(std::move(read.error()));
                lines = std::move(*read);
            } else {
                auto read = read_complete_lines(directory / file, file);
                if (!read) return std::unexpected(std::move(read.error()));
                lines = std::move(*read);
            }
            std::vector<ParsedSidecar> parsed;
            parsed.reserve(lines.size());
            const SidecarRecord* previous = nullptr;
            for (std::size_t index = 0; index < lines.size(); ++index) {
                auto record = parse_sidecar(lines[index].text, file, index + 1);
                if (!record) {
                    return std::unexpected(std::move(record.error()));
                }
                if (previous != nullptr &&
                        (record->seq < previous->seq ||
                                (record->seq == previous->seq && record->ordinal <= previous->ordinal))) {
                    return std::unexpected(corruption("Sidecar records are out of order in " + file));
                }
                parsed.push_back(ParsedSidecar{.record = *record, .start = lines[index].start});
                previous = &parsed.back().record;
            }
            parsed_files.emplace(file, std::move(parsed));
        }
        for (const auto& [file, lines] : parsed_files) {
            for (const ParsedSidecar& line : lines) {
                record_by_key.emplace(sidecar_key(file, line.record.seq, line.record.ordinal), &line.record);
            }
        }

        std::set<std::int64_t> retired;
        std::map<std::int64_t, bool> final_task_live;
        for (const auto& marker : markers) {
            for (const auto& operation : marker.writes) {
                if (operation.kind == MainKind::Task) final_task_live[operation.task.id.value] = false;
                if (operation.kind == MainKind::TaskSidecar) final_task_live[operation.task_id.value] = true;
            }
        }
        std::map<std::int64_t, std::pair<Seq, std::int64_t>> latest_bases;
        for (const MainMarker& marker : markers) {
            for (const MainOperation& operation : marker.writes) {
                if (operation.kind == MainKind::DocumentRetire) {
                    retired.insert(operation.document_id.value);
                } else if (operation.kind == MainKind::DocumentCreate || operation.kind == MainKind::DocumentCopy ||
                           operation.kind == MainKind::DocumentChange) {
                    const std::int64_t id = operation.kind == MainKind::DocumentChange
                                                    ? operation.document_id.value
                                                    : operation.document_record.id.value;
                    const std::string file = document_file(DocumentId{.value = id});
                    const auto found = record_by_key.find(sidecar_key(file, marker.seq, operation.ordinal));
                    if (found == record_by_key.end()) {
                        continue;
                    }
                    const SidecarRecord& record = *found->second;
                    if (record.payload.id.value != id || !record.payload.content.base) {
                        continue;
                    }
                    const auto previous = latest_bases.find(id);
                    if (previous == latest_bases.end() || marker.seq > previous->second.first ||
                            (marker.seq == previous->second.first && operation.ordinal > previous->second.second)) {
                        latest_bases[id] = {marker.seq, operation.ordinal};
                    }
                }
            }
        }
        const auto before_latest_base = [&](std::int64_t id, Seq seq, std::int64_t ordinal) {
            const auto found = latest_bases.find(id);
            return found != latest_bases.end() &&
                   (seq < found->second.first || (seq == found->second.first && ordinal < found->second.second));
        };

        std::set<std::string> confirmed;
        for (const MainMarker& marker : markers) {
            std::vector<StorageWrite> writes;
            for (const MainOperation& operation : marker.writes) {
                if (operation.kind == MainKind::Conversation) {
                    writes.push_back(StorageWrite{.type = "conversation", .conversation = operation.conversation});
                } else if (operation.kind == MainKind::Entry) {
                    writes.push_back(StorageWrite{.type = "entry", .entry = operation.entry});
                } else if (operation.kind == MainKind::Task) {
                    writes.push_back(StorageWrite{.type = "task", .task = operation.task});
                } else if (operation.kind == MainKind::TaskSidecar) {
                    const auto file = task_file(operation.task_id);
                    const auto key = sidecar_key(file, marker.seq, operation.ordinal);
                    if (confirmed.contains(key))
                        return std::unexpected(corruption("Sidecar record is confirmed more than once"));
                    const auto found = record_by_key.find(key);
                    const bool optional = !final_task_live.at(operation.task_id.value);
                    if (found == record_by_key.end()) {
                        if (!optional)
                            return std::unexpected(corruption("Missing confirmed sidecar record " + file +
                                                              " at sequence " + std::to_string(marker.seq)));
                    } else {
                        confirmed.insert(key);
                        const auto& task = found->second->payload.task;
                        if (!task || task->id.value != operation.task_id.value)
                            return std::unexpected(corruption(
                                    "Confirmed task sidecar data does not match commit " + std::to_string(marker.seq)));
                        if (!optional) writes.push_back(StorageWrite{.type = "task", .task = *task});
                    }
                } else if (operation.kind == MainKind::Submission) {
                    writes.push_back(StorageWrite{.type = "submission", .submission = operation.submission});
                } else if (operation.kind == MainKind::DocumentRetire) {
                    writes.push_back(StorageWrite{.type = "document.retire", .document_id = operation.document_id});
                } else {
                    const std::int64_t id = operation.kind == MainKind::DocumentChange
                                                    ? operation.document_id.value
                                                    : operation.document_record.id.value;
                    const std::string file = document_file(DocumentId{.value = id});
                    const bool reclaimed =
                            retired.contains(id) || before_latest_base(id, marker.seq, operation.ordinal);
                    const std::string key = sidecar_key(file, marker.seq, operation.ordinal);
                    if (confirmed.contains(key)) {
                        return std::unexpected(corruption("Sidecar record is confirmed more than once"));
                    }
                    const auto found = record_by_key.find(key);
                    if (found == record_by_key.end()) {
                        if (!reclaimed) {
                            return std::unexpected(corruption("Missing confirmed sidecar record " + file +
                                                              " at sequence " + std::to_string(marker.seq)));
                        }
                    } else {
                        confirmed.insert(key);
                    }
                    const SidecarRecord* record = found == record_by_key.end() ? nullptr : found->second;
                    if (record != nullptr && (record->payload.task.has_value() || record->payload.id.value != id)) {
                        return std::unexpected(corruption(
                                "Confirmed document sidecar data does not match commit " + std::to_string(marker.seq)));
                    }
                    const bool creates =
                            operation.kind == MainKind::DocumentCreate || operation.kind == MainKind::DocumentCopy;
                    if (creates) {
                        if (record != nullptr && !record->payload.content.base) {
                            return std::unexpected(corruption("Document creation lacks a confirmed base in commit " +
                                                              std::to_string(marker.seq)));
                        }
                        StorageWrite restored{
                                .type = operation.kind == MainKind::DocumentCopy ? "document.copy" : "document.create",
                                .document_record = operation.document_record};
                        if (operation.kind == MainKind::DocumentCopy) {
                            restored.document_copy_source = operation.document_copy_source;
                        }
                        if (record == nullptr) {
                            // A reclaimed creation still restores its record, so
                            // the retired incarnation stays addressable by ID.
                            restored.document_version = 1;
                            restored.document_value = chord::Json{chord::Json::Object{}};
                        } else {
                            restored.document_version = record->payload.content.version;
                            restored.document_value = record->payload.content.value;
                        }
                        writes.push_back(std::move(restored));
                    } else if (!reclaimed && record != nullptr) {
                        if (record->payload.content.base) {
                            writes.push_back(StorageWrite{.type = "document.change",
                                    .document_id = operation.document_id,
                                    .document_version = record->payload.content.version,
                                    .document_value = record->payload.content.value});
                        } else {
                            auto current = state.document(operation.document_id, DocumentPoint::current());
                            if (!current || !current->has_value()) {
                                return std::unexpected(corruption(
                                        "Invalid committed state at sequence " + std::to_string(marker.seq)));
                            }
                            auto applied = chord::apply_immutable((*current)->value, record->payload.content.ops);
                            if (!applied) {
                                return std::unexpected(corruption(
                                        "Invalid committed state at sequence " + std::to_string(marker.seq)));
                            }
                            writes.push_back(StorageWrite{.type = "document.change",
                                    .document_id = operation.document_id,
                                    .document_version = record->payload.content.version,
                                    .document_value = std::move(*applied),
                                    .document_ops = record->payload.content.ops});
                        }
                    }
                }
            }
            if (auto restored = state.restore(writes, marker.seq); !restored) {
                return std::unexpected(corruption("Invalid committed state at sequence " + std::to_string(marker.seq)));
            }
        }

        std::map<std::string, std::string> reclamations;
        for (auto& [file, lines] : parsed_files) {
            std::optional<std::size_t> unconfirmed_start;
            for (const ParsedSidecar& line : lines) {
                if (!confirmed.contains(sidecar_key(file, line.record.seq, line.record.ordinal))) {
                    unconfirmed_start = line.start;
                    break;
                }
            }
            if (unconfirmed_start.has_value()) {
                bool confirmed_after = false;
                for (const ParsedSidecar& line : lines) {
                    if (line.start > *unconfirmed_start &&
                            confirmed.contains(sidecar_key(file, line.record.seq, line.record.ordinal))) {
                        confirmed_after = true;
                        break;
                    }
                }
                if (confirmed_after) {
                    return std::unexpected(corruption("Confirmed record follows an unconfirmed tail in " + file));
                }
                for (const ParsedSidecar& line : lines) {
                    if (line.start >= *unconfirmed_start && line.record.seq <= previous_seq) {
                        return std::unexpected(
                                corruption("Unconfirmed sidecar record has a closed sequence in " + file));
                    }
                }
                const auto identity = task_identities.find(file);
                if (identity != task_identities.end()) {
                    auto offset = checked_offset(*unconfirmed_start);
                    if (!offset) {
                        return std::unexpected(std::move(offset.error()));
                    }
                    // Reopen rather than retain a descriptor per sidecar, and
                    // confirm the entry is still the file read a moment ago.
                    auto opened = open_task_file(directory_fd.get(), file, O_RDWR);
                    if (!opened) return std::unexpected(std::move(opened.error()));
                    if (!(opened->identity == identity->second)) {
                        return std::unexpected(corruption("JSONL task sidecar changed during recovery in " + file));
                    }
                    if (::ftruncate(opened->fd.get(), *offset) != 0) {
                        return std::unexpected(corruption("JSONL tail truncation of " + file + " failed"));
                    }
                } else {
                    std::filesystem::resize_file(directory / file, *unconfirmed_start, error);
                    if (error) {
                        return std::unexpected(corruption("JSONL tail truncation of " + file + " failed"));
                    }
                }
                lines.erase(std::remove_if(lines.begin(),
                                    lines.end(),
                                    [&](const ParsedSidecar& line) { return line.start >= *unconfirmed_start; }),
                        lines.end());
            }

            if (const auto task_id = task_id_from_file(file)) {
                const auto final = final_task_live.find(task_id->value);
                if (lines.empty() || (final != final_task_live.end() && !final->second)) {
                    // Published state is already established; a cleanup failure
                    // leaves the sidecar for a later retry instead of hiding it
                    // behind an open failure.
                    reclamations[file] = {};
                }
                continue;
            }
            const auto id = document_id_from_file(file);
            if (!id) {
                continue;
            }
            if (lines.empty() || retired.contains(id->value)) {
                reclamations[file] = {};
                continue;
            }
            const auto base = latest_bases.find(id->value);
            if (base == latest_bases.end()) {
                continue;
            }
            std::vector<ParsedSidecar> retained;
            for (const ParsedSidecar& line : lines) {
                if (line.record.seq > base->second.first ||
                        (line.record.seq == base->second.first && line.record.ordinal >= base->second.second)) {
                    retained.push_back(line);
                }
            }
            if (retained.size() == lines.size()) {
                continue;
            }
            if (retained.empty()) {
                reclamations[file] = {};
                continue;
            }
            std::string content;
            for (const ParsedSidecar& line : retained) {
                content += sidecar_line(line.record) + "\n";
            }
            reclamations[file] = std::move(content);
        }
        // Truncation is part of recovery and stays ungated; reclamation is
        // retryable maintenance applied once the plan is complete.
        this->task_identities = std::move(task_identities);
        reclaim_sidecars(reclamations);
        return {};
    }

    [[nodiscard]] support::Expected<Seq> commit(std::span<const StorageWrite> writes) {
        if (const auto error = unusable(); error.has_value()) {
            return std::unexpected(*error);
        }
        const Seq seq = state.next_seq();
        if (auto valid = state.validate(writes, seq); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        if (writes.empty()) {
            return seq;
        }

        // The sidecar of a current-only incarnation is replaced on a base and
        // removed on retirement; a rewindable one keeps every revision.
        std::set<std::int64_t> current_only_documents;
        for (const StorageWrite& write : writes) {
            if (write.type == "document.create" || write.type == "document.copy") {
                if (write.document_record.has_value() && document_is_current_only(*write.document_record)) {
                    current_only_documents.insert(write.document_record->id.value);
                }
                continue;
            }
            if (write.type != "document.change" && write.type != "document.retire") {
                continue;
            }
            if (!write.document_id.has_value()) {
                continue;
            }
            auto stored = state.document(*write.document_id, DocumentPoint::current());
            if (stored && (*stored).has_value() && document_is_current_only((**stored).record)) {
                current_only_documents.insert(write.document_id->value);
            }
        }

        EncodedCommit encoded = encode_commit(seq, writes, current_only_documents, directory);
        for (const auto& [file, content] : encoded.sidecars) {
            if (task_id_from_file(file)) {
                if (testing::fail_jsonl_io(io_event(testing::JsonlIoOperation::Append, file))) {
                    poisoned = true;
                    return std::unexpected(poison_error());
                }
                const auto known = task_identities.find(file);
                // With no observed entry, create exclusively: a sidecar that
                // reappears after reclamation must not be adopted. With an
                // observed entry, never create: the file must still exist.
                const int flags = known == task_identities.end() ? (O_WRONLY | O_APPEND | O_CREAT | O_EXCL)
                                                                 : (O_WRONLY | O_APPEND);
                auto opened = open_task_file(directory_fd.get(), file, flags);
                if (!opened) {
                    poisoned = true;
                    return std::unexpected(poison_error());
                }
                if (known != task_identities.end() && !(known->second == opened->identity)) {
                    poisoned = true;
                    return std::unexpected(poison_error());
                }
                task_identities.insert_or_assign(file, opened->identity);
                std::string_view remaining = content;
                while (!remaining.empty()) {
                    const auto count = ::write(opened->fd.get(), remaining.data(), remaining.size());
                    if (count < 0 && errno == EINTR) continue;
                    if (count <= 0) {
                        poisoned = true;
                        return std::unexpected(poison_error());
                    }
                    remaining.remove_prefix(static_cast<std::size_t>(count));
                }
                continue;
            }
            if (auto appended = append_file(directory / file, content); !appended) {
                poisoned = true;
                return std::unexpected(poison_error());
            }
        }
        if (options.fsync) {
            // Every sidecar is flushed in append order before the main marker
            // exists, so a flush failure can never publish a marker.
            for (const auto& sidecar : encoded.sidecars) {
                if (auto flushed = flush_sidecar(sidecar.first); !flushed) {
                    poisoned = true;
                    return std::unexpected(poison_error());
                }
            }
        }
        if (auto appended = append_file(directory / kMainFile, encoded.marker); !appended) {
            poisoned = true;
            return std::unexpected(poison_error());
        }
        if (auto applied = state.apply(writes, seq); !applied) {
            poisoned = true;
            return std::unexpected(std::move(applied.error()));
        }

        // The marker already published this state. Reclamation is retryable
        // maintenance and a failure here does not change the commit outcome.
        reclaim_sidecars(encoded.reclamations);
        return seq;
    }
};

JsonlStorage::JsonlStorage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

JsonlStorage::~JsonlStorage() = default;

support::Expected<std::unique_ptr<JsonlStorage>> JsonlStorage::open(
        const std::filesystem::path& directory, JsonlStorageOptions options) {
    auto impl = std::make_unique<Impl>(directory, options);
    if (auto recovered = impl->recover(); !recovered) {
        return std::unexpected(std::move(recovered.error()));
    }
    return std::unique_ptr<JsonlStorage>{new JsonlStorage{std::move(impl)}};
}

support::AsyncResult<Seq> JsonlStorage::commit(std::span<const StorageWrite> writes) {
    return support::AsyncResult<Seq>{impl_->commit(writes)};
}

support::Expected<std::int64_t> JsonlStorage::mint_id() {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.reserve_id();
}

support::Expected<std::optional<ConversationRecord>> JsonlStorage::conversation(ConversationId id) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.conversation(id);
}

support::Expected<std::optional<StoredEntry>> JsonlStorage::entry(EntryId id) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.entry(id);
}

support::Expected<std::vector<ConversationRecord>> JsonlStorage::scan_conversations(std::size_t limit) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.scan_conversations(limit);
}

support::Expected<std::vector<EntryRecord>> JsonlStorage::scan_entries(
        ConversationId conversation_id, std::size_t limit) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.scan_entries(conversation_id, limit);
}

support::Expected<std::vector<DocumentRecord>> JsonlStorage::scan_conversation_documents(
        ConversationId conversation_id, DocumentPoint at, std::size_t limit) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.scan_conversation_documents(conversation_id, at, limit);
}

support::Expected<std::optional<DocumentRecord>> JsonlStorage::find_document(
        std::string_view kind, const DocumentAddress& address, DocumentPoint at) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.find_document(kind, address, at);
}

support::Expected<std::optional<StoredDocument>> JsonlStorage::document(DocumentId id, DocumentPoint at) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.document(id, at);
}

support::Expected<std::optional<TaskRecord>> JsonlStorage::task(TaskId id) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.task(id);
}

support::Expected<std::vector<TaskRecord>> JsonlStorage::scan_tasks(std::size_t limit) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.scan_tasks(limit);
}

support::Expected<std::optional<SubmissionRecord>> JsonlStorage::submission(SubmissionId id) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.submission(id);
}

support::Expected<std::optional<SubmissionRecord>> JsonlStorage::submission_by_request(
        ConversationId conversation_id, std::string_view request_id) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.submission_by_request(conversation_id, request_id);
}

support::Expected<std::vector<SubmissionRecord>> JsonlStorage::scan_submissions(std::size_t limit) {
    if (const auto error = impl_->unusable(); error.has_value()) {
        return std::unexpected(*error);
    }
    return impl_->state.scan_submissions(limit);
}

support::ExpectedVoid JsonlStorage::close() {
    if (impl_->poisoned) {
        return {};
    }
    impl_->state.close();
    return {};
}

} // namespace cch::durable
