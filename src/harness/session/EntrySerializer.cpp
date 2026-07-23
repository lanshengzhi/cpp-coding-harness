#include "EntrySerializer.hpp"

#include "../../ai/glaze/AiJson.hpp"
#include "util/Json.hpp"
#include "util/Redactor.hpp"

#include <chrono>
#include <random>
#include <sstream>
#include <type_traits>
#include <utility>

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
    std::optional<std::string> timestamp;
    ai::glaze::MessageDto message;
};

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
    std::optional<std::vector<std::string>> tools;
    std::optional<std::vector<std::string>> activeToolNames;
};

struct CustomDto {
    std::string type{"custom"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string customType;
    glz::raw_json data{"null"};
};

using CustomMessageContentDto =
    std::variant<std::string, std::vector<ai::glaze::ContentDto>>;

struct CustomMessageDto {
    std::string type{"custom_message"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::string customType;
    CustomMessageContentDto content;
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
    std::optional<std::string> name;
};

struct LeafDto {
    std::string type{"leaf"};
    std::string id;
    std::optional<std::string> parentId;
    std::string timestamp;
    std::optional<std::string> targetId;
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
[[nodiscard]] util::Expected<T> entry_from_generic(
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
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm gm{};
    gmtime_r(&time, &gm);
    std::ostringstream oss;
    oss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
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
            dto.id.value_or(std::string{}),
            dto.timestamp.value_or(std::string{}),
            dto.cwd.value_or(std::string{}),
            dto.provider.value_or(std::string{}),
            dto.model.value_or(std::string{})};
    }
    return SessionMetadata{
        dto.sessionId.value_or(std::string{}),
        dto.createdAt.value_or(std::string{}),
        dto.workspace.value_or(std::string{}),
        dto.provider.value_or(std::string{}),
        dto.model.value_or(std::string{})};
}

[[nodiscard]] MessageEntryDto to_dto(
    std::string entry_id,
    const ai::MessageVariant& message,
    std::optional<std::string> parent_id = std::nullopt) {
    MessageEntryDto dto;
    dto.type = "message";
    dto.entryId = std::move(entry_id);
    dto.parentId = std::move(parent_id);
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

template <typename Dto>
void populate_tree_fields_from_dto(SessionEntry& entry, const Dto& dto) {
    entry.entry_id = dto.id;
    entry.parent_id = dto.parentId;
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

[[nodiscard]] std::optional<util::JsonValue> optional_json_field(
    const util::JsonValue& value,
    const std::string& key) {
    const auto* object = value.get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }

    const auto found = object->find(key);
    if (found == object->end()) {
        return std::nullopt;
    }

    return found->second;
}

[[nodiscard]] std::vector<std::string> active_tool_names_from_dto(const ActiveToolsChangeDto& dto) {
    if (dto.tools.has_value()) {
        return *dto.tools;
    }
    if (auto active_tool_names = dto.activeToolNames) {
        return *active_tool_names;
    }
    return {};
}

[[nodiscard]] util::Expected<CustomMessageEntryContent> custom_message_content_from_dto(
    const CustomMessageContentDto& content,
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

[[nodiscard]] CustomMessageContentDto custom_message_content_to_dto(
    CustomMessageEntryContent content) {
    if (auto* text = std::get_if<std::string>(&content)) {
        return CustomMessageContentDto{std::move(*text)};
    }

    const auto& blocks = std::get<std::vector<CustomMessageEntryContentBlock>>(content);
    std::vector<ai::glaze::ContentDto> dtos;
    dtos.reserve(blocks.size());
    for (const auto& block : blocks) {
        dtos.push_back(std::visit(
            [](const auto& concrete) { return ai::glaze::detail::to_dto(concrete); },
            block));
    }
    return CustomMessageContentDto{std::move(dtos)};
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

void redact_custom_message_entry_content(CustomMessageEntryContent& content) {
    if (auto* text = std::get_if<std::string>(&content)) {
        *text = util::redact_text(std::move(*text));
        return;
    }

    for (auto& block : std::get<std::vector<CustomMessageEntryContentBlock>>(content)) {
        if (auto* text = std::get_if<ai::TextContent>(&block)) {
            text->text = util::redact_text(std::move(text->text));
        }
    }
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

} // namespace

util::Expected<std::string> EntrySerializer::serialize_header(const SessionMetadata& metadata) const {
    return util::write_json(to_dto(metadata));
}

util::Expected<LoadedSession> EntrySerializer::parse_lines(const std::vector<std::string>& lines) const {
    LoadedSession loaded;
    std::size_t line_number = 0;
    bool saw_header = false;

    for (const auto& stored_line : lines) {
        ++line_number;
        if (stored_line.empty()) {
            continue;
        }

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

        auto payload = util::json_from_glaze(*generic);

        if (line_number == 1 && (*type == "header" || *type == "session")) {
            auto header = entry_from_generic<ReadHeaderDto>(*generic, stored_line, line_number);
            if (!header) {
                return std::unexpected(header.error());
            }
            loaded.metadata = from_dto(*header);

            SessionEntry entry;
            entry.kind = SessionEntryKind::Header;
            entry.raw_line = stored_line;
            entry.payload = std::move(payload);
            populate_tree_fields(entry, entry.payload);
            loaded.entries.push_back(std::move(entry));
            saw_header = true;
            continue;
        }

        auto kind = kind_from_type(*type);
        if (kind == SessionEntryKind::Message) {
            auto dto = entry_from_generic<MessageEntryDto>(*generic, stored_line, line_number);
            if (!dto) {
                return std::unexpected(dto.error());
            }
            auto message = ai::glaze::message_from_dto(dto->message, stored_line);
            if (!message) {
                return std::unexpected(message.error());
            }

            SessionEntry entry;
            entry.kind = SessionEntryKind::Message;
            entry.entry_id = !dto->entryId.empty() ? dto->entryId : dto->id;
            entry.parent_id = dto->parentId;
            entry.leaf_id = dto->leafId;
            if (dto->timestamp.has_value()) {
                entry.timestamp = parse_iso_timestamp_ms(*dto->timestamp);
            }
            entry.message = *message;
            entry.payload = std::move(payload);
            entry.raw_line = stored_line;
            loaded.messages.push_back(*message);
            loaded.entries.push_back(std::move(entry));
        } else {
            SessionEntry entry;
            entry.kind = kind;
            entry.raw_line = stored_line;
            entry.payload = std::move(payload);
            switch (kind) {
            case SessionEntryKind::ModelChange: {
                auto dto = entry_from_generic<ModelChangeDto>(*generic, stored_line, line_number);
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
                auto dto = entry_from_generic<ThinkingLevelChangeDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = ThinkingLevelChangeValue{.thinking_level = std::move(dto->thinkingLevel)};
                break;
            }
            case SessionEntryKind::ActiveToolsChange: {
                auto dto = entry_from_generic<ActiveToolsChangeDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = ActiveToolsChangeValue{.active_tool_names = active_tool_names_from_dto(*dto)};
                break;
            }
            case SessionEntryKind::Custom: {
                auto dto = entry_from_generic<CustomDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = CustomEntryValue{
                    .custom_type = std::move(dto->customType),
                    .data = optional_json_field(entry.payload, "data").value_or(util::JsonValue{}),
                };
                break;
            }
            case SessionEntryKind::CustomMessage: {
                auto dto = entry_from_generic<CustomMessageDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                auto content = custom_message_content_from_dto(dto->content, stored_line);
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
                auto dto = entry_from_generic<LabelDto>(*generic, stored_line, line_number);
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
                auto dto = entry_from_generic<CompactionDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = CompactionEntryValue{
                    .summary = std::move(dto->summary),
                    .first_kept_entry_id = std::move(dto->firstKeptEntryId),
                    .tokens_before = dto->tokensBefore,
                    .details = optional_json_field(entry.payload, "details"),
                    .from_hook = dto->fromHook,
                };
                break;
            }
            case SessionEntryKind::BranchSummary: {
                auto dto = entry_from_generic<BranchSummaryDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = BranchSummaryEntryValue{
                    .from_id = std::move(dto->fromId),
                    .summary = std::move(dto->summary),
                    .details = optional_json_field(entry.payload, "details"),
                    .from_hook = dto->fromHook,
                };
                break;
            }
            case SessionEntryKind::SessionInfo: {
                auto dto = entry_from_generic<SessionInfoDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = SessionInfoEntryValue{.name = std::move(dto->name).value_or(std::string{})};
                break;
            }
            case SessionEntryKind::Leaf: {
                auto dto = entry_from_generic<LeafDto>(*generic, stored_line, line_number);
                if (!dto) {
                    return std::unexpected(dto.error());
                }
                populate_tree_fields_from_dto(entry, *dto);
                entry.value = LeafEntryValue{.target_id = std::move(dto->targetId)};
                break;
            }
            case SessionEntryKind::Header:
            case SessionEntryKind::Message:
            case SessionEntryKind::Unknown:
                populate_tree_fields(entry, entry.payload);
                break;
            }
            loaded.entries.push_back(std::move(entry));
            if (kind == SessionEntryKind::Unknown) {
                loaded.unknown_lines.push_back(stored_line);
            }
        }
    }

    if (!saw_header) {
        return std::unexpected(session_error("session header is missing"));
    }

    return loaded;
}

util::Expected<std::string> EntrySerializer::serialize_message(const ai::MessageVariant& message) const {
    auto serialized = serialize_message_entry(message, std::nullopt);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    return std::move(serialized->line);
}

util::Expected<EntrySerializer::SerializedMessageEntry> EntrySerializer::serialize_message_entry(
    const ai::MessageVariant& message,
    std::optional<std::string> parent_id) const {
    auto redacted = redacted_message(message);
    auto entry_id = generate_entry_id();
    auto entry_json = util::write_json(to_dto(entry_id, redacted, std::move(parent_id)));
    if (!entry_json) {
        return std::unexpected(entry_json.error());
    }
    return SerializedMessageEntry{
        .line = *entry_json + '\n',
        .entry_id = std::move(entry_id),
    };
}

util::Expected<std::string> EntrySerializer::serialize_model_change(
    std::optional<std::string> parent_id,
    std::string provider,
    std::string model_id) const {
    ModelChangeDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.provider = std::move(provider);
    dto.modelId = std::move(model_id);

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_thinking_level_change(
    std::optional<std::string> parent_id,
    std::string thinking_level) const {
    ThinkingLevelChangeDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.thinkingLevel = std::move(thinking_level);

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_active_tools_change(
    std::optional<std::string> parent_id,
    std::vector<std::string> tools) const {
    ActiveToolsChangeDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.tools = std::move(tools);

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_custom_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    util::JsonValue data) const {
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

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_custom_message_entry(
    std::optional<std::string> parent_id,
    std::string custom_type,
    CustomMessageEntryContent content,
    bool display,
    std::optional<util::JsonValue> details) const {
    redact_custom_message_entry_content(content);
    if (details) {
        details = redact_json_value(*details);
    }

    CustomMessageDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.customType = std::move(custom_type);
    dto.content = custom_message_content_to_dto(std::move(content));
    dto.display = display;
    if (details) {
        auto details_json = util::write_json(*details);
        if (!details_json) {
            return std::unexpected(session_error("failed to serialize custom message details"));
        }
        dto.details = glz::raw_json{std::move(*details_json)};
    }

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_label_change(
    std::optional<std::string> parent_id,
    std::string target_id,
    std::optional<std::string> label) const {
    LabelDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.targetId = std::move(target_id);
    dto.label = std::move(label);

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_compaction(
    std::optional<std::string> parent_id,
    std::string summary,
    std::string first_kept_entry_id,
    std::size_t tokens_before,
    std::optional<util::JsonValue> details,
    std::optional<bool> from_hook) const {
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

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_branch_summary(
    std::optional<std::string> parent_id,
    std::string from_id,
    std::string summary,
    std::optional<util::JsonValue> details,
    std::optional<bool> from_hook) const {
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

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_session_info(
    std::optional<std::string> parent_id,
    std::string name) const {
    SessionInfoDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.name = std::move(name);

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

util::Expected<std::string> EntrySerializer::serialize_leaf(
    std::optional<std::string> parent_id,
    std::string target_id) const {
    LeafDto dto;
    dto.id = generate_entry_id();
    dto.parentId = std::move(parent_id);
    dto.timestamp = generate_iso_timestamp();
    dto.targetId = std::move(target_id);

    auto json_str = glz::write_json(dto);
    if (!json_str) {
        return std::unexpected(session_error("failed to serialize tree entry"));
    }
    return *json_str + '\n';
}

} // namespace cch::harness::session
