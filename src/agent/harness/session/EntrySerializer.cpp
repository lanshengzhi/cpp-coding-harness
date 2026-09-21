#include "EntrySerializer.hpp"

#include "agent/harness/session/EntryRedaction.hpp"
#include "agent/harness/session/RandomHex.hpp"
#include "agent/harness/session/SessionMessageJson.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <array>
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
    MessageDto message;
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

using CustomMessageContentDto = std::variant<std::string, std::vector<ContentDto>>;

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
    std::optional<std::vector<MessageDto>> retainedTail;
    std::optional<glz::raw_json> details;
    std::optional<UsageDto> usage;
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
    std::optional<UsageDto> usage;
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
    auto json = support::write_json(dto);
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

/// Parse one entry DTO from the raw line rather than from the `glz::generic`
/// already built for it. Glaze's generic -> DTO conversion re-serializes the
/// generic through its own default (unescaped) writer and reads that text back,
/// so a pi-captured `\uXXXX` control character failed with a `syntax_error`
/// reported against the intermediate text and not the line (#724).
template <typename T>
[[nodiscard]] support::Expected<T> entry_from_line(std::string_view line, std::size_t line_number) {
    auto parsed = support::read_json<T>(line);
    if (!parsed) {
        return std::unexpected(session_error("failed to parse session entry",
                "failed to parse session entry at line " + std::to_string(line_number) + ": " + parsed.error().detail));
    }
    return std::move(*parsed);
}

[[nodiscard]] std::string generate_entry_id() { return random_hex_id(8); }

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

/// The stored entry identity re-emitted as wire header fields: the
/// round-trip counterpart of the fresh append base.
struct RoundTripBase {
    std::string id;
    detail::NullableString parent_id;
    std::string timestamp;
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

template <typename Dto>
[[nodiscard]] support::Expected<EntrySerializer::SerializationResult> finish_fresh_entry(
        EntryBaseResult base, SessionEntryKind kind, const Dto& dto, SessionEntryValue value) {
    return finish_entry(serialize_tree_entry(dto), make_entry(std::move(base), kind, std::move(value)));
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

[[nodiscard]] support::Expected<detail::MessageEntryDto> to_dto(std::string entry_id,
        const ai::MessageVariant& message,
        std::optional<std::string> parent_id = std::nullopt,
        std::int64_t timestamp = 0) {
    detail::MessageEntryDto dto;
    dto.id = std::move(entry_id);
    dto.parentId = nullable_string(parent_id);
    dto.timestamp = format_iso_timestamp_ms(timestamp);
    auto message_dto = detail::to_message_dto(message);
    if (!message_dto) {
        return std::unexpected(message_dto.error());
    }
    dto.message = std::move(*message_dto);
    return dto;
}

/// Map a retained-tail message list into session DTOs, failing on the first
/// message the session writer cannot serialize (#665).
[[nodiscard]] support::Expected<std::vector<detail::MessageDto>> to_message_dtos(
        const std::vector<ai::MessageVariant>& messages) {
    std::vector<detail::MessageDto> dtos;
    dtos.reserve(messages.size());
    for (const auto& message : messages) {
        auto dto = detail::to_message_dto(message);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        dtos.push_back(std::move(*dto));
    }
    return dtos;
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

    const auto& content_dtos = std::get<std::vector<detail::ContentDto>>(content);
    std::vector<CustomMessageEntryContentBlock> converted;
    converted.reserve(content_dtos.size());
    for (const auto& dto : content_dtos) {
        auto block = detail::content_from_dto(dto, context);
        if (!block) {
            return std::unexpected(block.error());
        }
        if (auto* text = std::get_if<ai::TextContent>(&*block)) {
            converted.emplace_back(std::move(*text));
        } else if (auto* image = std::get_if<ai::ImageContent>(&*block)) {
            converted.emplace_back(std::move(*image));
        } else {
            return std::unexpected(detail::json_contract_error("unsupported custom_message content block",
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
    std::vector<detail::ContentDto> dtos;
    dtos.reserve(blocks.size());
    for (const auto& block : blocks) {
        dtos.push_back(std::visit([](const auto& concrete) { return detail::to_dto(concrete); }, block));
    }
    return detail::CustomMessageContentDto{std::move(dtos)};
}

/// The id/parent/timestamp triple every tree-entry DTO carries; one fill so
/// the wire header fields cannot drift between the serialize and round-trip
/// paths.
template <typename Dto> void attach_entry_header(const EntryBaseResult& base, Dto& dto) {
    dto.id = base.id;
    dto.parentId = nullable_string(base.parent_id);
    dto.timestamp = base.timestamp_text;
}

/// The round-trip base already carries wire-shaped fields.
template <typename Dto> void attach_entry_header(const RoundTripBase& base, Dto& dto) {
    dto.id = std::move(base.id);
    dto.parentId = std::move(base.parent_id);
    dto.timestamp = std::move(base.timestamp);
}

/// Serialize one optional JSON details value into the DTO's raw-JSON field,
/// naming the entry kind in the failure diagnostic.
[[nodiscard]] support::ExpectedVoid attach_details(
        const std::optional<support::JsonValue>& details, std::optional<glz::raw_json>& field, std::string_view what) {
    if (!details) {
        return {};
    }
    auto details_json = support::write_json(*details);
    if (!details_json) {
        return std::unexpected(session_error("failed to serialize " + std::string{what}));
    }
    field = glz::raw_json{std::move(*details_json)};
    return {};
}

/// Parse the DTO's optional usage payload, failing through the record's
/// parse channel when the usage shape violates the wire contract.
[[nodiscard]] support::Expected<std::optional<ai::Usage>> parse_usage_from_dto(
        const std::optional<detail::UsageDto>& usage, std::string_view line) {
    if (!usage) {
        return std::nullopt;
    }
    auto parsed = detail::usage_from_dto(*usage, line);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::optional{std::move(*parsed)};
}

/// Mirror an optional parsed usage into a DTO.
template <typename Dto> void attach_usage(Dto& dto, const std::optional<ai::Usage>& usage) {
    if (usage) {
        dto.usage = detail::to_dto(*usage);
    }
}

/// Parse a DTO's optional retained tail into message variants, failing on the
/// first message the session reader cannot parse (#665).
[[nodiscard]] support::Expected<std::optional<std::vector<ai::MessageVariant>>> parse_retained_tail_from_dto(
        const std::optional<std::vector<detail::MessageDto>>& retained_tail, std::string_view line) {
    if (!retained_tail) {
        return std::nullopt;
    }
    std::vector<ai::MessageVariant> tail;
    tail.reserve(retained_tail->size());
    for (const auto& message_dto : *retained_tail) {
        auto message = detail::message_from_dto(message_dto, line);
        if (!message) {
            return std::unexpected(message.error());
        }
        tail.push_back(std::move(*message));
    }
    return std::optional{std::move(tail)};
}

/// Mirror a retained-tail message list into session DTOs, failing on the
/// first message the session writer cannot serialize (#665).
[[nodiscard]] support::Expected<std::optional<std::vector<detail::MessageDto>>> retained_tail_to_dtos(
        const std::optional<std::vector<ai::MessageVariant>>& retained_tail) {
    if (!retained_tail) {
        return std::nullopt;
    }
    auto tail = to_message_dtos(*retained_tail);
    if (!tail) {
        return std::unexpected(tail.error());
    }
    return std::optional{std::move(*tail)};
}

/// Parse one non-message tree entry from its DTO: the shared sequence of
/// parse, tree-field population, and value assembly, so the per-kind arms
/// carry only their own value construction.
template <typename Dto, typename Build>
[[nodiscard]] support::Expected<SessionEntry> parse_typed_entry(std::string_view line,
        std::size_t line_number,
        SessionEntryKind kind,
        support::JsonValue payload,
        Build build) {
    auto dto = entry_from_line<Dto>(line, line_number);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    SessionEntry entry;
    entry.kind = kind;
    entry.raw_line = line;
    entry.payload = std::move(payload);
    populate_tree_fields_from_dto(entry, *dto);
    if (auto built = build(*dto, entry); !built) {
        return std::unexpected(built.error());
    }
    return entry;
}

/// The message entry's own id fallback (`id` or legacy `entryId`) and
/// `leafId` mirror make it the one arm that cannot share the typed template.
[[nodiscard]] support::Expected<SessionEntry> parse_message_entry(
        std::string_view line, std::size_t line_number, support::JsonValue payload) {
    auto dto = entry_from_line<detail::MessageEntryDto>(line, line_number);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    auto message = detail::message_from_dto(dto->message, line);
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
                auto header = entry_from_line<detail::ReadHeaderDto>(stored_line, line_number);
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

/// Defined below `parse_entry`: the typed-kind dispatch arm.
[[nodiscard]] support::Expected<SessionEntry> parse_typed_tree_entry(
        std::string_view line, std::size_t line_number, SessionEntryKind kind, support::JsonValue payload);

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
    const auto kind = kind_from_type(*type);
    if (kind == SessionEntryKind::Message) {
        return parse_message_entry(line, line_number, std::move(payload));
    }
    if (kind == SessionEntryKind::Header || kind == SessionEntryKind::Unknown) {
        SessionEntry entry;
        entry.kind = kind;
        entry.raw_line = line;
        entry.payload = std::move(payload);
        populate_tree_fields(entry, entry.payload);
        return entry;
    }
    return parse_typed_tree_entry(line, line_number, kind, std::move(payload));
}

/// The ten typed kinds dispatch through the shared `parse_typed_entry`
/// sequence; each arm contributes only its own value construction, and the
/// fallible arms keep their exact wire diagnostics (#665).
[[nodiscard]] support::Expected<SessionEntry> parse_typed_tree_entry(
        std::string_view line, std::size_t line_number, SessionEntryKind kind, support::JsonValue payload) {
    switch (kind) {
    case SessionEntryKind::ModelChange:
        return parse_typed_entry<detail::ModelChangeDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::ModelChangeDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = ModelChangeValue{
                            .provider = std::move(dto.provider),
                            .model_id = std::move(dto.modelId),
                    };
                    return {};
                });
    case SessionEntryKind::ThinkingLevelChange:
        return parse_typed_entry<detail::ThinkingLevelChangeDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::ThinkingLevelChangeDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = ThinkingLevelChangeValue{.thinking_level = std::move(dto.thinkingLevel)};
                    return {};
                });
    case SessionEntryKind::ActiveToolsChange:
        return parse_typed_entry<detail::ActiveToolsChangeDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::ActiveToolsChangeDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = ActiveToolsChangeValue{.active_tool_names = active_tool_names_from_dto(dto)};
                    return {};
                });
    case SessionEntryKind::Custom:
        return parse_typed_entry<detail::CustomDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::CustomDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = CustomEntryValue{
                            .custom_type = std::move(dto.customType),
                            .data = optional_json_field(entry.payload, "data"),
                    };
                    return {};
                });
    case SessionEntryKind::CustomMessage:
        return parse_typed_entry<detail::CustomMessageDto>(line,
                line_number,
                kind,
                std::move(payload),
                [&line](detail::CustomMessageDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    auto content = custom_message_content_from_dto(dto.content, line);
                    if (!content) {
                        return std::unexpected(content.error());
                    }
                    entry.value = CustomMessageEntryValue{
                            .custom_type = std::move(dto.customType),
                            .content = std::move(*content),
                            .display = dto.display,
                            .details = optional_json_field(entry.payload, "details"),
                    };
                    return {};
                });
    case SessionEntryKind::Label:
        return parse_typed_entry<detail::LabelDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::LabelDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = LabelEntryValue{
                            .target_id = std::move(dto.targetId),
                            .label = std::move(dto.label),
                    };
                    return {};
                });
    case SessionEntryKind::Compaction:
        return parse_typed_entry<detail::CompactionDto>(line,
                line_number,
                kind,
                std::move(payload),
                [&line](detail::CompactionDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    auto retained_tail = parse_retained_tail_from_dto(dto.retainedTail, line);
                    if (!retained_tail) {
                        return std::unexpected(retained_tail.error());
                    }
                    auto usage = parse_usage_from_dto(dto.usage, line);
                    if (!usage) {
                        return std::unexpected(usage.error());
                    }
                    entry.value = CompactionEntryValue{
                            .summary = std::move(dto.summary),
                            .first_kept_entry_id = std::move(dto.firstKeptEntryId),
                            .tokens_before = dto.tokensBefore,
                            .retained_tail = std::move(*retained_tail),
                            .details = optional_json_field(entry.payload, "details"),
                            .usage = std::move(*usage),
                            .from_hook = dto.fromHook,
                    };
                    return {};
                });
    case SessionEntryKind::BranchSummary:
        return parse_typed_entry<detail::BranchSummaryDto>(line,
                line_number,
                kind,
                std::move(payload),
                [&line](detail::BranchSummaryDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    auto usage = parse_usage_from_dto(dto.usage, line);
                    if (!usage) {
                        return std::unexpected(usage.error());
                    }
                    entry.value = BranchSummaryEntryValue{
                            .from_id = std::move(dto.fromId),
                            .summary = std::move(dto.summary),
                            .details = optional_json_field(entry.payload, "details"),
                            .usage = std::move(*usage),
                            .from_hook = dto.fromHook,
                    };
                    return {};
                });
    case SessionEntryKind::SessionInfo:
        return parse_typed_entry<detail::SessionInfoDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::SessionInfoDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = SessionInfoEntryValue{.name = std::move(dto.name)};
                    return {};
                });
    case SessionEntryKind::Leaf:
        return parse_typed_entry<detail::LeafDto>(line,
                line_number,
                kind,
                std::move(payload),
                [](detail::LeafDto& dto, SessionEntry& entry) -> support::ExpectedVoid {
                    entry.value = LeafEntryValue{.target_id = optional_from_nullable(dto.targetId)};
                    return {};
                });
    case SessionEntryKind::Header:
    case SessionEntryKind::Message:
    case SessionEntryKind::Unknown:
        break;
    }
    return std::unexpected(session_error("unreachable kind", "parse_typed_tree_entry reached a non-typed kind"));
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
    auto entry_dto = to_dto(base.id, redacted, base.parent_id, base.timestamp_ms);
    if (!entry_dto) {
        return std::unexpected(entry_dto.error());
    }
    auto entry_json = support::write_json(*entry_dto);
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
    attach_entry_header(base, dto);
    dto.provider = provider;
    dto.modelId = model_id;
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::ModelChange,
            dto,
            ModelChangeValue{.provider = std::move(provider), .model_id = std::move(model_id)});
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) const {
    auto base = fresh_entry_base(parent_id);
    detail::ThinkingLevelChangeDto dto;
    attach_entry_header(base, dto);
    dto.thinkingLevel = thinking_level;
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::ThinkingLevelChange,
            dto,
            ThinkingLevelChangeValue{.thinking_level = std::move(thinking_level)});
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_active_tools_change(
    std::optional<std::string> parent_id,
    std::vector<std::string> tools) const {
    auto base = fresh_entry_base(parent_id);
    detail::ActiveToolsChangeDto dto;
    attach_entry_header(base, dto);
    dto.activeToolNames = tools;
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::ActiveToolsChange,
            dto,
            ActiveToolsChangeValue{.active_tool_names = std::move(tools)});
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    std::optional<support::JsonValue> data) const {
    auto base = fresh_entry_base(parent_id);
    detail::CustomDto dto;
    attach_entry_header(base, dto);
    dto.customType = custom_type;
    if (auto attached = attach_details(data, dto.data, "custom entry data"); !attached) {
        return std::unexpected(attached.error());
    }
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::Custom,
            dto,
            CustomEntryValue{
                    .custom_type = std::move(custom_type),
                    .data = std::move(data),
            });
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
    attach_entry_header(base, dto);
    dto.customType = custom_type;
    dto.content = custom_message_content_to_dto(CustomMessageEntryContent(content));
    dto.display = display;
    if (auto attached = attach_details(details, dto.details, "custom message details"); !attached) {
        return std::unexpected(attached.error());
    }
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::CustomMessage,
            dto,
            CustomMessageEntryValue{
                    .custom_type = std::move(custom_type),
                    .content = std::move(content),
                    .display = display,
                    .details = std::move(details),
            });
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) const {
    auto base = fresh_entry_base(parent_id);
    detail::LabelDto dto;
    attach_entry_header(base, dto);
    dto.targetId = target_id;
    dto.label = label;
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::Label,
            dto,
            LabelEntryValue{.target_id = std::move(target_id), .label = std::move(label)});
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
    // Redact before the value is mirrored into the DTO and the live entry, so
    // the line and the live tree agree exactly as they do on the message path.
    redact_summary_entry_text(value.summary, value.details);
    if (value.retained_tail) {
        // The retained tail inherits `redacted_message`'s exemption list
        // unchanged (notably ADR 0028's User Bash text); this path adds none.
        for (auto& message : *value.retained_tail) {
            message = redacted_message(message);
        }
    }
    // Deliberately raw, one reason per field: `first_kept_entry_id` is an entry
    // identifier (the exemption recorded for `BranchSummaryMessage.from_id`),
    // and `tokens_before`, `usage`, and `from_hook` carry no text.
    detail::CompactionDto dto;
    attach_entry_header(base, dto);
    dto.summary = value.summary;
    dto.firstKeptEntryId = value.first_kept_entry_id;
    dto.tokensBefore = value.tokens_before;
    if (auto tail = retained_tail_to_dtos(value.retained_tail); !tail) {
        return std::unexpected(tail.error());
    } else if (tail->has_value()) {
        dto.retainedTail = std::move(**tail);
    }
    if (auto attached = attach_details(value.details, dto.details, "compaction details"); !attached) {
        return std::unexpected(attached.error());
    }
    attach_usage(dto, value.usage);
    dto.fromHook = value.from_hook;
    return finish_fresh_entry(std::move(base), SessionEntryKind::Compaction, dto, std::move(value));
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<support::JsonValue> details,
    std::optional<bool> from_hook,
    std::optional<ai::Usage> usage) const {
    redact_summary_entry_text(summary, details);
    // Deliberately raw, one reason per field: `from_id` is an entry identifier
    // (the exemption recorded for `BranchSummaryMessage.from_id`), and `usage`
    // and `from_hook` carry no text.
    auto base = fresh_entry_base(parent_id);
    detail::BranchSummaryDto dto;
    attach_entry_header(base, dto);
    dto.fromId = from_id;
    dto.summary = summary;
    if (auto attached = attach_details(details, dto.details, "branch summary details"); !attached) {
        return std::unexpected(attached.error());
    }
    attach_usage(dto, usage);
    dto.fromHook = from_hook;
    return finish_fresh_entry(std::move(base),
            SessionEntryKind::BranchSummary,
            dto,
            BranchSummaryEntryValue{
                    .from_id = std::move(from_id),
                    .summary = std::move(summary),
                    .details = std::move(details),
                    .usage = std::move(usage),
                    .from_hook = from_hook,
            });
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_session_info(
    std::optional<std::string> parent_id,
    std::optional<std::string> name) const {
    auto base = fresh_entry_base(parent_id);
    detail::SessionInfoDto dto;
    attach_entry_header(base, dto);
    dto.name = name;
    return finish_fresh_entry(
            std::move(base), SessionEntryKind::SessionInfo, dto, SessionInfoEntryValue{.name = std::move(name)});
}

support::Expected<EntrySerializer::SerializationResult> EntrySerializer::serialize_leaf(
    std::optional<std::string> parent_id,
    std::optional<std::string> target_id) const {
    auto base = fresh_entry_base(parent_id);
    detail::LeafDto dto;
    attach_entry_header(base, dto);
    dto.targetId = nullable_string(target_id);
    return finish_fresh_entry(
            std::move(base), SessionEntryKind::Leaf, dto, LeafEntryValue{.target_id = std::move(target_id)});
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
    const auto base = RoundTripBase{
            entry.entry_id,
            nullable_string(entry.parent_id),
            format_iso_timestamp_ms(static_cast<std::int64_t>(entry.timestamp)),
    };

    switch (entry.kind) {
    case SessionEntryKind::Message: {
        if (!entry.message.has_value()) {
            return std::unexpected(session_error("message entry has no message value"));
        }
        detail::MessageEntryDto dto;
        attach_entry_header(base, dto);
        auto message_dto = detail::to_message_dto(*entry.message);
        if (!message_dto) {
            return std::unexpected(message_dto.error());
        }
        dto.message = std::move(*message_dto);
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::ModelChange: {
        const auto& value = std::get<ModelChangeValue>(entry.value);
        detail::ModelChangeDto dto;
        attach_entry_header(base, dto);
        dto.provider = value.provider;
        dto.modelId = value.model_id;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::ThinkingLevelChange: {
        const auto& value = std::get<ThinkingLevelChangeValue>(entry.value);
        detail::ThinkingLevelChangeDto dto;
        attach_entry_header(base, dto);
        dto.thinkingLevel = value.thinking_level;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::ActiveToolsChange: {
        const auto& value = std::get<ActiveToolsChangeValue>(entry.value);
        detail::ActiveToolsChangeDto dto;
        attach_entry_header(base, dto);
        dto.activeToolNames = value.active_tool_names;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Custom: {
        const auto& value = std::get<CustomEntryValue>(entry.value);
        detail::CustomDto dto;
        attach_entry_header(base, dto);
        dto.customType = value.custom_type;
        if (auto attached = attach_details(value.data, dto.data, "custom entry data"); !attached) {
            return std::unexpected(attached.error());
        }
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::CustomMessage: {
        const auto& value = std::get<CustomMessageEntryValue>(entry.value);
        detail::CustomMessageDto dto;
        attach_entry_header(base, dto);
        dto.customType = value.custom_type;
        dto.content = custom_message_content_to_dto(value.content);
        dto.display = value.display;
        if (auto attached = attach_details(value.details, dto.details, "custom message details"); !attached) {
            return std::unexpected(attached.error());
        }
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Label: {
        const auto& value = std::get<LabelEntryValue>(entry.value);
        detail::LabelDto dto;
        attach_entry_header(base, dto);
        dto.targetId = value.target_id;
        dto.label = value.label;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Compaction: {
        const auto& value = std::get<CompactionEntryValue>(entry.value);
        detail::CompactionDto dto;
        attach_entry_header(base, dto);
        dto.summary = value.summary;
        dto.firstKeptEntryId = value.first_kept_entry_id;
        dto.tokensBefore = value.tokens_before;
        if (auto tail = retained_tail_to_dtos(value.retained_tail); !tail) {
            return std::unexpected(tail.error());
        } else if (tail->has_value()) {
            dto.retainedTail = std::move(**tail);
        }
        if (auto attached = attach_details(value.details, dto.details, "compaction details"); !attached) {
            return std::unexpected(attached.error());
        }
        attach_usage(dto, value.usage);
        dto.fromHook = value.from_hook;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::BranchSummary: {
        const auto& value = std::get<BranchSummaryEntryValue>(entry.value);
        detail::BranchSummaryDto dto;
        attach_entry_header(base, dto);
        dto.fromId = value.from_id;
        dto.summary = value.summary;
        if (auto attached = attach_details(value.details, dto.details, "branch summary details"); !attached) {
            return std::unexpected(attached.error());
        }
        attach_usage(dto, value.usage);
        dto.fromHook = value.from_hook;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::SessionInfo: {
        const auto& value = std::get<SessionInfoEntryValue>(entry.value);
        detail::SessionInfoDto dto;
        attach_entry_header(base, dto);
        dto.name = value.name;
        return serialize_tree_entry(dto);
    }
    case SessionEntryKind::Leaf: {
        const auto& value = std::get<LeafEntryValue>(entry.value);
        detail::LeafDto dto;
        attach_entry_header(base, dto);
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
