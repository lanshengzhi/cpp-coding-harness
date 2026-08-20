#include "EntrySerializer.hpp"

#include "ai/glaze/AiJson.hpp"
#include "support/Json.hpp"
#include "ai/Redactor.hpp"

#include <chrono>
#include <random>
#include <sstream>
#include <type_traits>
#include <utility>

namespace cch::harness::session {
// The session-entry DTOs must have external linkage: Glaze reflects them
// through glz::detail::external, which Clang rejects for anonymous-namespace
// types (issue #487; the Clang conformance build).
namespace detail {

/// Wire nullable string: `std::monostate` serializes as explicit JSON `null`
/// (pi root entries carry `parentId: null`; a root leaf carries
/// `targetId: null`), engaged string as the value. Absent fields are
/// represented by `std::optional` nullopt, which Glaze omits — this keeps
/// pi's null-vs-missing distinction on the wire.
using NullableString = std::variant<std::monostate, std::string>;

struct WriteHeaderDto {
    std::string type{"session"};
    int version{3};
    std::string id;
    std::string timestamp;
    std::string cwd;
    std::optional<std::string> parentSession;
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
    std::optional<std::string> parentSession;
};

// All entry DTOs declare fields in pi's exact wire order (pi builds entries
// as `{...{id, parentId, timestamp}, type, ...fields}`, so JSON.stringify
// emits `id, parentId, timestamp, type, <fields>`); Glaze writes members in
// declaration order, which is what makes the golden byte-identical.

struct MessageEntryDto {
    std::string id;
    NullableString parentId;
    std::optional<std::string> timestamp;
    std::string type{"message"};
    ai::glaze::MessageDto message;
    // Legacy read tolerance: pre-pi C++ files carried `entryId`/`leafId`.
    std::optional<std::string> entryId;
    std::optional<std::string> leafId;
};

struct ModelChangeDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"model_change"};
    std::string provider;
    std::string modelId;
};

struct ThinkingLevelChangeDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"thinking_level_change"};
    std::string thinkingLevel;
};

struct ActiveToolsChangeDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"active_tools_change"};
    std::vector<std::string> activeToolNames;
    // Legacy read tolerance: pre-pi C++ files carried `tools`.
    std::optional<std::vector<std::string>> tools;
};

struct CustomDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"custom"};
    std::string customType;
    // pi `data?` — omitted when absent, explicit `null` when the caller
    // passed null; both must round-trip.
    std::optional<glz::raw_json> data;
};

using CustomMessageContentDto =
    std::variant<std::string, std::vector<ai::glaze::ContentDto>>;

struct CustomMessageDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"custom_message"};
    std::string customType;
    CustomMessageContentDto content;
    bool display{true};
    std::optional<glz::raw_json> details;
};

struct LabelDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"label"};
    std::string targetId;
    std::optional<std::string> label;
};

struct CompactionDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"compaction"};
    std::string summary;
    std::optional<std::string> firstKeptEntryId;
    std::size_t tokensBefore{0};
    std::optional<std::vector<ai::glaze::MessageDto>> retainedTail;
    std::optional<glz::raw_json> details;
    std::optional<ai::glaze::UsageDto> usage;
    std::optional<bool> fromHook;
};

struct BranchSummaryDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"branch_summary"};
    std::string fromId;
    std::string summary;
    std::optional<glz::raw_json> details;
    std::optional<ai::glaze::UsageDto> usage;
    std::optional<bool> fromHook;
};

struct SessionInfoDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"session_info"};
    std::optional<std::string> name;
};

struct LeafDto {
    std::string id;
    NullableString parentId;
    std::string timestamp;
    std::string type{"leaf"};
    NullableString targetId;
};

} // namespace detail

namespace {

[[nodiscard]] support::Error session_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Session, std::move(message), std::move(detail));
}

template <typename Dto>
[[nodiscard]] support::Expected<std::string> serialize_tree_entry(const Dto& dto) {
    auto json = glz::write_json(dto);
    if (!json) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json + '\n';
}

[[nodiscard]] support::Expected<std::string> entry_type(const glz::generic& parsed, std::size_t line_number) {
    // `get_if` (not the throwing `get`) so a scalar/array root reports the
    // same missing-type outcome as a root without a "type" member.
    const auto* object = parsed.get_if<glz::generic::object_t>();
    if (object == nullptr) {
        return std::unexpected(session_error(
            "session entry missing type",
            "session entry missing type at line " + std::to_string(line_number)));
    }
    const auto found = object->find("type");
    if (found == object->end()) {
        return std::unexpected(session_error(
            "session entry missing type",
            "session entry missing type at line " + std::to_string(line_number)));
    }
    const auto* type = found->second.get_if<std::string>();
    if (type == nullptr) {
        return std::unexpected(session_error(
            "session entry missing type",
            "session entry missing type at line " + std::to_string(line_number)));
    }
    return *type;
}

template <typename T>
[[nodiscard]] support::Expected<T> entry_from_generic(
    const glz::generic& value,
    std::string_view line,
    std::size_t line_number) {
    auto parsed = glz::read_json<T>(value);
    if (!parsed) {
        return std::unexpected(session_error(
            "failed to parse session entry",
            "failed to parse session entry at line " + std::to_string(line_number) + ": " +
                glz::format_error(parsed.error(), line)));
    }
    // The engaged check above makes this a non-throwing move.
    return std::move(*parsed);
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

[[nodiscard]] std::int64_t ms_since_epoch() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Inverse of `parse_iso_timestamp_ms`: render an epoch-millisecond timestamp
/// as pi's exact ISO-8601 UTC form with three-digit milliseconds (`Z` suffix).
[[nodiscard]] std::string format_iso_timestamp_ms(std::int64_t ms) {
    const std::int64_t days = ms >= 0 ? ms / 86400000 : (ms - 86399999) / 86400000;
    const std::int64_t day_ms = ms - days * 86400000;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = y + (month <= 2 ? 1 : 0);
    const unsigned hour = static_cast<unsigned>(day_ms / 3600000);
    const unsigned minute = static_cast<unsigned>((day_ms / 60000) % 60);
    const unsigned second = static_cast<unsigned>((day_ms / 1000) % 60);
    const unsigned millis = static_cast<unsigned>(day_ms % 1000);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << month
        << '-' << std::setw(2) << day << 'T' << std::setw(2) << hour << ':'
        << std::setw(2) << minute << ':' << std::setw(2) << second << '.'
        << std::setw(3) << millis << 'Z';
    return oss.str();
}

/// Shared identity for one appended entry: a single generated id and a
/// single `now` used for both the wire timestamp text and the mirrored
/// entry's epoch-millisecond timestamp, so the live tree and the line can
/// never disagree about when the entry was written.
struct EntryBaseResult {
    std::string id;
    std::optional<std::string> parent_id;
    ai::TimestampMs timestamp_ms;
    std::string timestamp_text;
};

[[nodiscard]] EntryBaseResult fresh_entry_base(const std::optional<std::string>& parent_id) {
    const auto now = ms_since_epoch();
    return EntryBaseResult{
        .id = generate_entry_id(),
        .parent_id = parent_id,
        .timestamp_ms = static_cast<ai::TimestampMs>(now),
        .timestamp_text = format_iso_timestamp_ms(now),
    };
}

[[nodiscard]] SessionEntry make_entry(
    EntryBaseResult base,
    SessionEntryKind kind,
    SessionEntryValue value) {
    SessionEntry entry;
    entry.kind = kind;
    entry.entry_id = std::move(base.id);
    entry.parent_id = std::move(base.parent_id);
    entry.timestamp = base.timestamp_ms;
    entry.value = std::move(value);
    return entry;
}

[[nodiscard]] support::Expected<EntrySerializer::SerializationResult> finish_entry(
    support::Expected<std::string> line,
    SessionEntry entry) {
    if (!line) {
        return std::unexpected(line.error());
    }
    return EntrySerializer::SerializationResult{
        .line = std::move(*line),
        .entry = std::move(entry),
    };
}

[[nodiscard]] detail::NullableString nullable_string(const std::optional<std::string>& value) {
    if (value) {
        return detail::NullableString{*value};
    }
    return detail::NullableString{std::monostate{}};
}

[[nodiscard]] std::optional<std::string> optional_from_nullable(const detail::NullableString& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> parse_fixed_int(std::string_view text, std::size_t offset, std::size_t count) {
    if (offset + count > text.size()) {
        return std::nullopt;
    }

    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char ch = text[offset + i];
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

[[nodiscard]] std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] ai::TimestampMs parse_iso_timestamp_ms(std::string_view timestamp) {
    if (timestamp.size() < 20 ||
        timestamp[4] != '-' ||
        timestamp[7] != '-' ||
        (timestamp[10] != 'T' && timestamp[10] != ' ') ||
        timestamp[13] != ':' ||
        timestamp[16] != ':') {
        return 0;
    }

    const auto year = parse_fixed_int(timestamp, 0, 4);
    const auto month = parse_fixed_int(timestamp, 5, 2);
    const auto day = parse_fixed_int(timestamp, 8, 2);
    const auto hour = parse_fixed_int(timestamp, 11, 2);
    const auto minute = parse_fixed_int(timestamp, 14, 2);
    const auto second = parse_fixed_int(timestamp, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return 0;
    }

    std::size_t zone_pos = 19;
    int millis = 0;
    if (timestamp[zone_pos] == '.') {
        ++zone_pos;
        int scale = 100;
        int digits = 0;
        while (zone_pos < timestamp.size() && timestamp[zone_pos] >= '0' && timestamp[zone_pos] <= '9') {
            if (digits < 3) {
                millis += (timestamp[zone_pos] - '0') * scale;
                scale /= 10;
                ++digits;
            }
            ++zone_pos;
        }
    }

    if (zone_pos >= timestamp.size() || timestamp[zone_pos] != 'Z') {
        return 0;
    }

    const auto days = days_from_civil(
        *year,
        static_cast<unsigned>(*month),
        static_cast<unsigned>(*day));
    const auto total_seconds =
        (((days * 24 + *hour) * 60 + *minute) * 60 + *second);
    return total_seconds * 1000 + millis;
}

[[nodiscard]] detail::WriteHeaderDto to_dto(const SessionMetadata& metadata) {
    detail::WriteHeaderDto dto{
        "session",
        3,
        metadata.session_id,
        metadata.created_at,
        metadata.workspace.string(),
        std::nullopt,
        metadata.provider,
        metadata.model,
    };
    if (metadata.parent_session) {
        dto.parentSession = metadata.parent_session->string();
    }
    return dto;
}

[[nodiscard]] SessionMetadata from_dto(const detail::ReadHeaderDto& dto) {
    if (dto.type == "session") {
        SessionMetadata metadata{
            dto.id.value_or(std::string{}),
            dto.timestamp.value_or(std::string{}),
            dto.cwd.value_or(std::string{}),
            dto.provider.value_or(std::string{}),
            dto.model.value_or(std::string{})};
        if (dto.parentSession && !dto.parentSession->empty()) {
            metadata.parent_session = *dto.parentSession;
        }
        return metadata;
    }
    SessionMetadata metadata{
        dto.sessionId.value_or(std::string{}),
        dto.createdAt.value_or(std::string{}),
        dto.workspace.value_or(std::string{}),
        dto.provider.value_or(std::string{}),
        dto.model.value_or(std::string{})};
    if (dto.parentSession && !dto.parentSession->empty()) {
        metadata.parent_session = *dto.parentSession;
    }
    return metadata;
}

[[nodiscard]] detail::MessageEntryDto to_dto(
    std::string entry_id,
    const ai::MessageVariant& message,
    std::optional<std::string> parent_id = std::nullopt,
    std::int64_t timestamp = 0) {
    detail::MessageEntryDto dto;
    dto.id = std::move(entry_id);
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = format_iso_timestamp_ms(timestamp);
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
    const support::JsonValue::object_t& object,
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

void populate_tree_fields(SessionEntry& entry, const support::JsonValue& value) {
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        if (auto id = optional_string_field(*object, "entryId")) {
            entry.entry_id = *id;
        } else if (auto id = optional_string_field(*object, "id")) {
            entry.entry_id = *id;
        }
        entry.parent_id = optional_string_field(*object, "parentId");
        entry.leaf_id = optional_string_field(*object, "leafId");
    }
}

template <typename Dto>
void populate_tree_fields_from_dto(SessionEntry& entry, const Dto& dto) {
    entry.entry_id = dto.id;
    entry.parent_id = optional_from_nullable(dto.parentId);
    if constexpr (requires { dto.timestamp; }) {
        using TimestampField = std::remove_cvref_t<decltype(dto.timestamp)>;
        if constexpr (std::is_same_v<TimestampField, std::optional<std::string>>) {
            if (dto.timestamp.has_value()) {
                entry.timestamp = parse_iso_timestamp_ms(*dto.timestamp);
            }
        } else {
            entry.timestamp = parse_iso_timestamp_ms(dto.timestamp);
        }
    }
}

[[nodiscard]] std::optional<support::JsonValue> optional_json_field(
    const support::JsonValue& value,
    const std::string& key) {
    const auto* object = value.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }

    const auto found = object->find(key);
    if (found == object->end()) {
        return std::nullopt;
    }

    return found->second;
}

[[nodiscard]] std::vector<std::string> active_tool_names_from_dto(const detail::ActiveToolsChangeDto& dto) {
    if (dto.tools.has_value()) {
        return *dto.tools;
    }
    return dto.activeToolNames;
}

[[nodiscard]] support::Expected<CustomMessageEntryContent> custom_message_content_from_dto(
    const detail::CustomMessageContentDto& content,
    std::string_view context) {
    if (const auto* text = std::get_if<std::string>(&content)) {
        return CustomMessageEntryContent{*text};
    }

    const auto& content_dtos = std::get<std::vector<ai::glaze::ContentDto>>(content);
    std::vector<CustomMessageEntryContentBlock> converted;
    converted.reserve(content_dtos.size());
    for (const auto& dto : content_dtos) {
        auto block = ai::glaze::detail::content_from_dto(dto, context);
        if (!block) {
            return std::unexpected(block.error());
        }
        if (auto* text = std::get_if<ai::TextContent>(&*block)) {
            converted.emplace_back(std::move(*text));
        } else if (auto* image = std::get_if<ai::ImageContent>(&*block)) {
            converted.emplace_back(std::move(*image));
        } else {
            return std::unexpected(ai::glaze::detail::json_contract_error(
                "unsupported custom_message content block",
                "custom_message content accepts only text and image blocks",
                context));
        }
    }
    return CustomMessageEntryContent{std::move(converted)};
}

[[nodiscard]] detail::CustomMessageContentDto custom_message_content_to_dto(
    CustomMessageEntryContent content) {
    if (auto* text = std::get_if<std::string>(&content)) {
        return detail::CustomMessageContentDto{std::move(*text)};
    }

    const auto& blocks = std::get<std::vector<CustomMessageEntryContentBlock>>(content);
    std::vector<ai::glaze::ContentDto> dtos;
    dtos.reserve(blocks.size());
    for (const auto& block : blocks) {
        dtos.push_back(std::visit(
            [](const auto& concrete) { return ai::glaze::detail::to_dto(concrete); },
            block));
    }
    return detail::CustomMessageContentDto{std::move(dtos)};
}

[[nodiscard]] support::JsonValue redact_json_value(const support::JsonValue& value) {
    if (const auto* text = value.get_if<std::string>()) {
        return support::JsonValue{ai::redact_text(*text)};
    }
    if (const auto* values = value.get_if<support::JsonValue::array_t>()) {
        support::JsonValue::array_t redacted;
        redacted.reserve(values->size());
        for (const auto& item : *values) {
            redacted.push_back(redact_json_value(item));
        }
        return support::JsonValue{std::move(redacted)};
    }
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        support::JsonValue::object_t redacted;
        for (const auto& [key, item] : *object) {
            if (ai::looks_secret_key(key)) {
                redacted.emplace(key, support::JsonValue{std::string{ai::kRedactionMarker}});
            } else {
                redacted.emplace(key, redact_json_value(item));
            }
        }
        return support::JsonValue{std::move(redacted)};
    }
    return value;
}

void redact_content(ai::Content& content) {
    std::visit(
        [](auto& block) {
            using T = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<T, ai::TextContent>) {
                block.text = ai::redact_text(std::move(block.text));
            } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
                block.thinking = ai::redact_text(std::move(block.thinking));
            }
        },
        content);
}

void redact_custom_message_entry_content(CustomMessageEntryContent& content) {
    if (auto* text = std::get_if<std::string>(&content)) {
        *text = ai::redact_text(std::move(*text));
        return;
    }

    for (auto& block : std::get<std::vector<CustomMessageEntryContentBlock>>(content)) {
        if (auto* text = std::get_if<ai::TextContent>(&block)) {
            text->text = ai::redact_text(std::move(text->text));
        }
    }
}

void redact_assistant_content(ai::AssistantContent& content) {
    std::visit(
        [](auto& block) {
            using T = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<T, ai::TextContent>) {
                block.text = ai::redact_text(std::move(block.text));
            } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
                block.thinking = ai::redact_text(std::move(block.thinking));
            } else if constexpr (std::is_same_v<T, ai::ToolCallContent>) {
                if (block.arguments) {
                    block.arguments = redact_json_value(*block.arguments);
                }
                block.raw_arguments = ai::redact_text(std::move(block.raw_arguments));
                if (block.argument_error) {
                    block.argument_error = ai::redact_text(std::move(*block.argument_error));
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
                concrete.content = ai::redact_text(std::move(concrete.content));
            } else if constexpr (std::is_same_v<T, ai::AssistantMessage>) {
                for (auto& block : concrete.content) {
                    redact_assistant_content(block);
                }
                if (concrete.error_message) {
                    concrete.error_message = ai::redact_text(std::move(*concrete.error_message));
                }
            } else if constexpr (std::is_same_v<T, ai::BashExecutionMessage>) {
                concrete.output = ai::redact_text(std::move(concrete.output));
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
            } else if constexpr (std::is_same_v<T, ai::UserMessage>) {
                if (auto* text = std::get_if<std::string>(&concrete.content)) {
                    *text = ai::redact_text(std::move(*text));
                } else {
                    for (auto& block :
                         std::get<std::vector<ai::Content>>(concrete.content)) {
                        redact_content(block);
                    }
                }
            } else {
                // ToolResultMessage
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

} // namespace

support::Expected<std::string> EntrySerializer::serialize_header(const SessionMetadata& metadata) const {
    return support::write_json(to_dto(metadata));
}

support::Expected<LoadedSession> EntrySerializer::parse_lines(const std::vector<std::string>& lines) const {
    LoadedSession loaded;
    std::size_t line_number = 0;
    bool saw_header = false;

    for (const auto& stored_line : lines) {
        ++line_number;
        if (stored_line.empty()) {
            continue;
        }

        if (line_number == 1) {
            auto generic = glz::read_json<glz::generic>(stored_line);
            if (!generic) {
                return std::unexpected(session_error(
                    "malformed JSONL",
                    "malformed JSONL at line " + std::to_string(line_number) + ": " +
                        glz::format_error(generic.error(), stored_line)));
            }

            auto type = entry_type(*generic, line_number);
            if (!type) {
                return std::unexpected(type.error());
            }

            if (*type == "header" || *type == "session") {
                auto header = entry_from_generic<detail::ReadHeaderDto>(*generic, stored_line, line_number);
                if (!header) {
                    return std::unexpected(header.error());
                }
                loaded.metadata = from_dto(*header);

                SessionEntry entry;
                entry.kind = SessionEntryKind::Header;
                entry.raw_line = stored_line;
                entry.payload = support::json_from_glaze(*generic);
                populate_tree_fields(entry, entry.payload);
                loaded.entries.push_back(std::move(entry));
                saw_header = true;
                continue;
            }
        }

        auto entry = parse_entry(stored_line, line_number);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        if (entry->kind == SessionEntryKind::Message && entry->message.has_value()) {
            loaded.messages.push_back(*entry->message);
        }
        if (entry->kind == SessionEntryKind::Unknown) {
            loaded.unknown_lines.push_back(stored_line);
        }
        loaded.entries.push_back(std::move(*entry));
    }

    if (!saw_header) {
        return std::unexpected(session_error("session header is missing"));
    }

    return loaded;
}

support::Expected<SessionEntry> EntrySerializer::parse_entry(
    std::string_view line,
    std::size_t line_number) const {
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

    auto payload = support::json_from_glaze(*generic);
    auto kind = kind_from_type(*type);
    if (kind == SessionEntryKind::Message) {
        auto dto = entry_from_generic<detail::MessageEntryDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        auto message = ai::glaze::message_from_dto(dto->message, line);
        if (!message) {
            return std::unexpected(message.error());
        }

        SessionEntry entry;
        entry.kind = SessionEntryKind::Message;
        entry.entry_id = !dto->id.empty() ? dto->id : dto->entryId.value_or("");
        entry.parent_id = optional_from_nullable(dto->parentId);
        entry.leaf_id = dto->leafId;
        if (dto->timestamp.has_value()) {
            entry.timestamp = parse_iso_timestamp_ms(*dto->timestamp);
        }
        entry.message = *message;
        entry.payload = std::move(payload);
        entry.raw_line = line;
        return entry;
    }

    SessionEntry entry;
    entry.kind = kind;
    entry.raw_line = line;
    entry.payload = std::move(payload);
    switch (kind) {
    case SessionEntryKind::ModelChange: {
        auto dto = entry_from_generic<detail::ModelChangeDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = ModelChangeValue{
            .provider = std::move(dto->provider),
            .model_id = std::move(dto->modelId),
        };
        break;
    }
    case SessionEntryKind::ThinkingLevelChange: {
        auto dto = entry_from_generic<detail::ThinkingLevelChangeDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = ThinkingLevelChangeValue{.thinking_level = std::move(dto->thinkingLevel)};
        break;
    }
    case SessionEntryKind::ActiveToolsChange: {
        auto dto = entry_from_generic<detail::ActiveToolsChangeDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = ActiveToolsChangeValue{.active_tool_names = active_tool_names_from_dto(*dto)};
        break;
    }
    case SessionEntryKind::Custom: {
        auto dto = entry_from_generic<detail::CustomDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = CustomEntryValue{
            .custom_type = std::move(dto->customType),
            .data = optional_json_field(entry.payload, "data"),
        };
        break;
    }
    case SessionEntryKind::CustomMessage: {
        auto dto = entry_from_generic<detail::CustomMessageDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        auto content = custom_message_content_from_dto(dto->content, line);
        if (!content) {
            return std::unexpected(content.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = CustomMessageEntryValue{
            .custom_type = std::move(dto->customType),
            .content = std::move(*content),
            .display = dto->display,
            .details = optional_json_field(entry.payload, "details"),
        };
        break;
    }
    case SessionEntryKind::Label: {
        auto dto = entry_from_generic<detail::LabelDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = LabelEntryValue{
            .target_id = std::move(dto->targetId),
            .label = std::move(dto->label),
        };
        break;
    }
    case SessionEntryKind::Compaction: {
        auto dto = entry_from_generic<detail::CompactionDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        std::optional<std::vector<ai::MessageVariant>> retained_tail;
        if (dto->retainedTail.has_value()) {
            std::vector<ai::MessageVariant> tail;
            tail.reserve(dto->retainedTail->size());
            for (const auto& message_dto : *dto->retainedTail) {
                auto message = ai::glaze::message_from_dto(message_dto, line);
                if (!message) {
                    return std::unexpected(message.error());
                }
                tail.push_back(std::move(*message));
            }
            retained_tail = std::move(tail);
        }
        std::optional<ai::Usage> usage;
        if (dto->usage.has_value()) {
            auto parsed_usage = ai::glaze::detail::usage_from_dto(*dto->usage, line);
            if (!parsed_usage) {
                return std::unexpected(parsed_usage.error());
            }
            usage = std::move(*parsed_usage);
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = CompactionEntryValue{
            .summary = std::move(dto->summary),
            .first_kept_entry_id = std::move(dto->firstKeptEntryId),
            .tokens_before = dto->tokensBefore,
            .retained_tail = std::move(retained_tail),
            .details = optional_json_field(entry.payload, "details"),
            .usage = std::move(usage),
            .from_hook = dto->fromHook,
        };
        break;
    }
    case SessionEntryKind::BranchSummary: {
        auto dto = entry_from_generic<detail::BranchSummaryDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        std::optional<ai::Usage> usage;
        if (dto->usage.has_value()) {
            auto parsed_usage = ai::glaze::detail::usage_from_dto(*dto->usage, line);
            if (!parsed_usage) {
                return std::unexpected(parsed_usage.error());
            }
            usage = std::move(*parsed_usage);
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = BranchSummaryEntryValue{
            .from_id = std::move(dto->fromId),
            .summary = std::move(dto->summary),
            .details = optional_json_field(entry.payload, "details"),
            .usage = std::move(usage),
            .from_hook = dto->fromHook,
        };
        break;
    }
    case SessionEntryKind::SessionInfo: {
        auto dto = entry_from_generic<detail::SessionInfoDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = SessionInfoEntryValue{.name = std::move(dto->name)};
        break;
    }
    case SessionEntryKind::Leaf: {
        auto dto = entry_from_generic<detail::LeafDto>(*generic, line, line_number);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        populate_tree_fields_from_dto(entry, *dto);
        entry.value = LeafEntryValue{.target_id = optional_from_nullable(dto->targetId)};
        break;
    }
    case SessionEntryKind::Header:
    case SessionEntryKind::Message:
    case SessionEntryKind::Unknown:
        populate_tree_fields(entry, entry.payload);
        break;
    }
    return entry;
}

support::Expected<std::string> EntrySerializer::serialize_message(const ai::MessageVariant& message) const {
    auto serialized = serialize_message_entry(message, std::nullopt);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    return std::move(serialized->line);
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_message_entry(
    const ai::MessageVariant& message,
    std::optional<std::string> parent_id) const {
    auto redacted = redacted_message(message);
    auto base = fresh_entry_base(parent_id);
    auto entry_json = support::write_json(
        to_dto(base.id, redacted, base.parent_id, base.timestamp_ms));
    if (!entry_json) {
        return std::unexpected(entry_json.error());
    }
    SessionEntry entry;
    entry.kind = SessionEntryKind::Message;
    entry.entry_id = std::move(base.id);
    entry.parent_id = std::move(base.parent_id);
    entry.timestamp = base.timestamp_ms;
    entry.message = std::move(redacted);
    return SerializationResult{
        .line = *entry_json + '\n',
        .entry = std::move(entry),
    };
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) const {
    auto base = fresh_entry_base(parent_id);
    detail::ModelChangeDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.provider = provider;
    dto.modelId = model_id;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::ModelChange,
            ModelChangeValue{
                .provider = std::move(provider),
                .model_id = std::move(model_id),
            }));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) const {
    auto base = fresh_entry_base(parent_id);
    detail::ThinkingLevelChangeDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.thinkingLevel = thinking_level;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::ThinkingLevelChange,
            ThinkingLevelChangeValue{.thinking_level = std::move(thinking_level)}));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_active_tools_change(
    std::optional<std::string> parent_id,
    std::vector<std::string> tools) const {
    auto base = fresh_entry_base(parent_id);
    detail::ActiveToolsChangeDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.activeToolNames = tools;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::ActiveToolsChange,
            ActiveToolsChangeValue{.active_tool_names = std::move(tools)}));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    std::optional<support::JsonValue> data) const {
    auto base = fresh_entry_base(parent_id);
    detail::CustomDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.customType = custom_type;
    if (data) {
        auto data_json = support::write_json(*data);
        if (!data_json) {
            return std::unexpected(session_error("failed to serialize custom entry data"));
        }
        dto.data = glz::raw_json{std::move(*data_json)};
    }
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::Custom,
            CustomEntryValue{
                .custom_type = std::move(custom_type),
                .data = std::move(data),
            }));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_custom_message_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    CustomMessageEntryContent content,
    bool display,
    std::optional<support::JsonValue> details) const {
    redact_custom_message_entry_content(content);
    if (details) {
        details = redact_json_value(*details);
    }

    auto base = fresh_entry_base(parent_id);
    detail::CustomMessageDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.customType = custom_type;
    dto.content = custom_message_content_to_dto(CustomMessageEntryContent(content));
    dto.display = display;
    if (details) {
        auto details_json = support::write_json(*details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize custom message details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::CustomMessage,
            CustomMessageEntryValue{
                .custom_type = std::move(custom_type),
                .content = std::move(content),
                .display = display,
                .details = std::move(details),
            }));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) const {
    auto base = fresh_entry_base(parent_id);
    detail::LabelDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.targetId = target_id;
    dto.label = label;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::Label,
            LabelEntryValue{
                .target_id = std::move(target_id),
                .label = std::move(label),
            }));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_compaction(
    std::optional<std::string> parent_id,
    CompactionEntryValue value) const {
    auto base = fresh_entry_base(parent_id);
    // The wire omits an empty retained tail; the mirrored entry does the
    // same so live-tree and reload reads agree.
    if (value.retained_tail && value.retained_tail->empty()) {
        value.retained_tail.reset();
    }
    detail::CompactionDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.summary = value.summary;
    dto.firstKeptEntryId = value.first_kept_entry_id;
    dto.tokensBefore = value.tokens_before;
    if (value.retained_tail) {
        std::vector<ai::glaze::MessageDto> tail;
        tail.reserve(value.retained_tail->size());
        for (const auto& message : *value.retained_tail) {
            tail.push_back(ai::glaze::to_message_dto(message));
        }
        dto.retainedTail = std::move(tail);
    }
    if (value.details) {
        auto details_json = support::write_json(*value.details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize compaction details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }
    if (value.usage) {
        dto.usage = ai::glaze::detail::to_dto(*value.usage);
    }
    dto.fromHook = value.from_hook;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::Compaction,
            std::move(value)));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook,
    std::optional<ai::Usage> usage) const {
    auto base = fresh_entry_base(parent_id);
    detail::BranchSummaryDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.fromId = from_id;
    dto.summary = summary;
    if (details) {
        auto details_json = support::write_json(*details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize branch summary details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }
    if (usage) {
        dto.usage = ai::glaze::detail::to_dto(*usage);
    }
    dto.fromHook = from_hook;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::BranchSummary,
            BranchSummaryEntryValue{
                .from_id = std::move(from_id),
                .summary = std::move(summary),
                .details = std::move(details),
                .usage = std::move(usage),
                .from_hook = from_hook,
            }));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_session_info(
    std::optional<std::string> parent_id,
    std::optional<std::string> name) const {
    auto base = fresh_entry_base(parent_id);
    detail::SessionInfoDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.name = name;
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::SessionInfo,
            SessionInfoEntryValue{.name = std::move(name)}));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) const {
    auto base = fresh_entry_base(parent_id);
    detail::LeafDto dto;
    dto.id = base.id;
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = base.timestamp_text;
    dto.targetId = nullable_string(target_id);
    return finish_entry(
        serialize_tree_entry(dto),
        make_entry(
            std::move(base),
            SessionEntryKind::Leaf,
            LeafEntryValue{.target_id = std::move(target_id)}));
}

std::string EntrySerializer::new_entry_id() {
    return generate_entry_id();
}

ai::TimestampMs EntrySerializer::now_timestamp_ms() {
    return ms_since_epoch();
}

support::Expected<std::string> EntrySerializer::serialize_entry(const SessionEntry& entry) const {
    // Round-trip writer: re-emits a parsed entry byte-identically (pi field
    // presence, null-vs-missing, ordering) using the stored id/timestamp so a
    // parsed pi line round-trips exactly. No redaction here — redaction is
    // append-time policy; round-trip fidelity is the contract.
    struct IdOnly {
        std::string id;
        detail::NullableString parentId;
        std::string timestamp;
    };
    const auto base_fields = [&]() {
        return IdOnly{
            entry.entry_id,
            nullable_string(entry.parent_id),
            format_iso_timestamp_ms(static_cast<std::int64_t>(entry.timestamp)),
        };
    };
    auto base = base_fields();

    switch (entry.kind) {
    case SessionEntryKind::Message: {
        if (!entry.message.has_value()) {
            return std::unexpected(session_error("message entry has no message value"));
        }
        detail::MessageEntryDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.message = ai::glaze::to_message_dto(*entry.message);
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::ModelChange: {
        const auto& value = std::get<ModelChangeValue>(entry.value);
        detail::ModelChangeDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.provider = value.provider;
        dto.modelId = value.model_id;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::ThinkingLevelChange: {
        const auto& value = std::get<ThinkingLevelChangeValue>(entry.value);
        detail::ThinkingLevelChangeDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.thinkingLevel = value.thinking_level;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::ActiveToolsChange: {
        const auto& value = std::get<ActiveToolsChangeValue>(entry.value);
        detail::ActiveToolsChangeDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.activeToolNames = value.active_tool_names;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Custom: {
        const auto& value = std::get<CustomEntryValue>(entry.value);
        detail::CustomDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.customType = value.custom_type;
        if (value.data) {
            auto data_json = support::write_json(*value.data);
            if (!data_json) {
                return std::unexpected(session_error("failed to serialize custom entry data"));
            }
            dto.data = glz::raw_json{std::move(*data_json)};
        }
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::CustomMessage: {
        const auto& value = std::get<CustomMessageEntryValue>(entry.value);
        detail::CustomMessageDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.customType = value.custom_type;
        dto.content = custom_message_content_to_dto(value.content);
        dto.display = value.display;
        if (value.details) {
            auto details_json = support::write_json(*value.details);
            if (!details_json) {
                return std::unexpected(session_error("failed to serialize custom message details"));
            }
            dto.details = glz::raw_json{std::move(*details_json)};
        }
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Label: {
        const auto& value = std::get<LabelEntryValue>(entry.value);
        detail::LabelDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.targetId = value.target_id;
        dto.label = value.label;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Compaction: {
        const auto& value = std::get<CompactionEntryValue>(entry.value);
        detail::CompactionDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.summary = value.summary;
        dto.firstKeptEntryId = value.first_kept_entry_id;
        dto.tokensBefore = value.tokens_before;
        if (value.retained_tail.has_value()) {
            std::vector<ai::glaze::MessageDto> tail;
            tail.reserve(value.retained_tail->size());
            for (const auto& message : *value.retained_tail) {
                tail.push_back(ai::glaze::to_message_dto(message));
            }
            dto.retainedTail = std::move(tail);
        }
        if (value.details) {
            auto details_json = support::write_json(*value.details);
            if (!details_json) {
                return std::unexpected(session_error("failed to serialize compaction details"));
            }
            dto.details = glz::raw_json{std::move(*details_json)};
        }
        if (value.usage) {
            dto.usage = ai::glaze::detail::to_dto(*value.usage);
        }
        dto.fromHook = value.from_hook;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::BranchSummary: {
        const auto& value = std::get<BranchSummaryEntryValue>(entry.value);
        detail::BranchSummaryDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.fromId = value.from_id;
        dto.summary = value.summary;
        if (value.details) {
            auto details_json = support::write_json(*value.details);
            if (!details_json) {
                return std::unexpected(session_error("failed to serialize branch summary details"));
            }
            dto.details = glz::raw_json{std::move(*details_json)};
        }
        if (value.usage) {
            dto.usage = ai::glaze::detail::to_dto(*value.usage);
        }
        dto.fromHook = value.from_hook;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::SessionInfo: {
        const auto& value = std::get<SessionInfoEntryValue>(entry.value);
        detail::SessionInfoDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.name = value.name;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Leaf: {
        const auto& value = std::get<LeafEntryValue>(entry.value);
        detail::LeafDto dto;
        dto.id = std::move(base.id);
        dto.parentId = std::move(base.parentId);
        dto.timestamp = std::move(base.timestamp);
        dto.targetId = nullable_string(value.target_id);
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Header:
    case SessionEntryKind::Unknown:
        return std::unexpected(session_error(
            "cannot round-trip entry",
            "kind does not serialize as a tree entry"));
    }
    return std::unexpected(session_error("unreachable"));
}

} // namespace cch::harness::session
