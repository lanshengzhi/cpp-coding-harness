#pragma once

#include "../../../include/cch/ai/Context.hpp"
#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/ai/Usage.hpp"
#include "ToolSchemaDtos.hpp"
#include "../../util/Json.hpp"

#include <glaze/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::ai::glaze {

struct UsageCostDto {
    std::optional<double> input;
    std::optional<double> output;
    std::optional<double> cacheRead;
    std::optional<double> cacheWrite;
    std::optional<double> total;
};

struct UsageDto {
    std::optional<std::int64_t> input;
    std::optional<std::int64_t> output;
    std::optional<std::int64_t> cacheRead;
    std::optional<std::int64_t> cacheWrite;
    std::optional<std::int64_t> cacheWrite1h;
    std::optional<std::int64_t> reasoning;
    std::optional<std::int64_t> totalTokens;
    std::optional<UsageCostDto> cost;
};

struct DiagnosticErrorInfoDto {
    std::optional<std::string> name;
    std::string message;
    std::optional<std::string> stack;
    std::optional<std::string> code;
};

struct DiagnosticEntryDto {
    std::string type;
    std::int64_t timestamp{};
    std::optional<DiagnosticErrorInfoDto> error;
    std::optional<glz::generic> details;
};

struct ContentDto {
    std::string type;
    std::optional<std::string> text;
    std::optional<std::string> textSignature;
    std::optional<std::string> thinking;
    std::optional<std::string> thinkingSignature;
    std::optional<bool> redacted;
    std::optional<std::string> data;
    std::optional<std::string> mimeType;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<glz::generic> arguments;
    std::optional<std::string> rawArguments;
    std::optional<std::string> thoughtSignature;
    std::optional<bool> argumentsValid;
    std::optional<std::string> argumentError;
};

struct MessageDto {
    std::string role;
    std::optional<std::vector<ContentDto>> content;
    std::optional<std::string> api;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> responseModel;
    std::optional<std::string> responseId;
    std::optional<UsageDto> usage;
    std::optional<std::string> stopReason;
    std::optional<std::string> errorMessage;
    std::optional<std::vector<DiagnosticEntryDto>> diagnostics;
    std::optional<std::string> toolCallId;
    std::optional<std::string> toolName;
    std::optional<glz::generic> details;
    std::optional<bool> isError;
    // Extended message type fields
    std::optional<std::string> command;
    std::optional<std::string> output;
    std::optional<std::int64_t> exitCode;
    std::optional<bool> cancelled;
    std::optional<bool> truncated;
    std::optional<std::string> fullOutputPath;
    std::optional<bool> excludeFromContext;
    std::optional<std::string> customType;
    std::optional<bool> display;
    std::optional<std::string> summary;
    std::optional<std::string> fromId;
    std::optional<std::int64_t> tokensBefore;
    std::int64_t timestamp{};
};

struct ContextDto {
    std::optional<std::string> systemPrompt;
    std::optional<std::string> model;
    std::vector<MessageDto> messages;
    std::optional<std::vector<FunctionToolDto>> tools;
};

[[nodiscard]] inline std::string stop_reason_to_json(AssistantStopReason reason) {
    return stop_reason_to_string(reason);
}

[[nodiscard]] inline std::optional<AssistantStopReason> stop_reason_from_json(std::string_view reason) {
    if (reason == "stop") {
        return AssistantStopReason::Stop;
    }
    if (reason == "length") {
        return AssistantStopReason::Length;
    }
    if (reason == "toolUse") {
        return AssistantStopReason::ToolUse;
    }
    if (reason == "error") {
        return AssistantStopReason::Error;
    }
    if (reason == "aborted") {
        return AssistantStopReason::Aborted;
    }
    return std::nullopt;
}

namespace detail {

inline constexpr std::int64_t kMinimumRealUnixEpochMilliseconds = 1'000'000'000'000;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

[[nodiscard]] inline util::Error json_contract_error(std::string message, std::string detail, std::string_view context) {
    return util::make_error(
        util::ErrorCode::JsonParse,
        std::move(message),
        std::move(detail),
        context.empty() ? std::nullopt : std::optional<std::string>{std::string(context)});
}

[[nodiscard]] inline util::Expected<std::vector<Content>> content_from_dto(
    const std::vector<ContentDto>& content,
    std::string_view context);

template <typename T>
[[nodiscard]] inline util::ExpectedVoid require_field(
    const std::optional<T>& field,
    std::string_view discriminator,
    std::string_view field_name,
    std::string_view context) {
    if (field) {
        return {};
    }
    return std::unexpected(json_contract_error(
        "missing required JSON field",
        std::string{"missing required field '"} + std::string(field_name) + "' for " + std::string(discriminator),
        context));
}

[[nodiscard]] inline util::ExpectedVoid require_non_empty_string(
    const std::optional<std::string>& field,
    std::string_view discriminator,
    std::string_view field_name,
    std::string_view context) {
    if (auto required = require_field(field, discriminator, field_name, context); !required) {
        return required;
    }
    if (!field->empty()) {
        return {};
    }
    return std::unexpected(json_contract_error(
        "empty required JSON field",
        std::string{"required field '"} + std::string(field_name) + "' for " +
            std::string(discriminator) + " must not be empty",
        context));
}

[[nodiscard]] inline util::Expected<std::vector<Content>> required_content_from_dto(
    const std::optional<std::vector<ContentDto>>& content,
    std::string_view role,
    std::string_view context) {
    if (!content) {
        return std::unexpected(json_contract_error(
            "missing required JSON field",
            std::string{"missing required field 'content' for "} + std::string(role),
            context));
    }
    return content_from_dto(*content, context);
}

[[nodiscard]] inline UsageDto to_dto(const Usage& usage) {
    return UsageDto{
        .input = usage.input,
        .output = usage.output,
        .cacheRead = usage.cache_read,
        .cacheWrite = usage.cache_write,
        .cacheWrite1h = usage.cache_write_1h,
        .reasoning = usage.reasoning,
        .totalTokens = usage.total_tokens,
        .cost = UsageCostDto{
            .input = usage.cost.input,
            .output = usage.cost.output,
            .cacheRead = usage.cost.cache_read,
            .cacheWrite = usage.cost.cache_write,
            .total = usage.cost.total,
        },
    };
}

[[nodiscard]] inline util::Expected<Usage> usage_from_dto(
    const UsageDto& dto,
    std::string_view context) {
    if (auto required = require_field(dto.input, "assistant usage", "usage.input", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(dto.output, "assistant usage", "usage.output", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(dto.cacheRead, "assistant usage", "usage.cacheRead", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(dto.cacheWrite, "assistant usage", "usage.cacheWrite", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(dto.totalTokens, "assistant usage", "usage.totalTokens", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(dto.cost, "assistant usage", "usage.cost", context); !required) {
        return std::unexpected(required.error());
    }

    const auto& cost = *dto.cost;
    if (auto required = require_field(cost.input, "assistant usage cost", "usage.cost.input", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(cost.output, "assistant usage cost", "usage.cost.output", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(cost.cacheRead, "assistant usage cost", "usage.cost.cacheRead", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(cost.cacheWrite, "assistant usage cost", "usage.cost.cacheWrite", context); !required) {
        return std::unexpected(required.error());
    }
    if (auto required = require_field(cost.total, "assistant usage cost", "usage.cost.total", context); !required) {
        return std::unexpected(required.error());
    }

    return Usage{
        .input = *dto.input,
        .output = *dto.output,
        .cache_read = *dto.cacheRead,
        .cache_write = *dto.cacheWrite,
        .cache_write_1h = dto.cacheWrite1h,
        .reasoning = dto.reasoning,
        .total_tokens = *dto.totalTokens,
        .cost = UsageCost{
            .input = *cost.input,
            .output = *cost.output,
            .cache_read = *cost.cacheRead,
            .cache_write = *cost.cacheWrite,
            .total = *cost.total,
        },
    };
}

[[nodiscard]] inline ContentDto to_dto(const TextContent& content) {
    ContentDto dto;
    dto.type = "text";
    dto.text = content.text;
    dto.textSignature = content.text_signature;
    return dto;
}

[[nodiscard]] inline ContentDto to_dto(const ThinkingContent& content) {
    ContentDto dto;
    dto.type = "thinking";
    dto.thinking = content.thinking;
    dto.thinkingSignature = content.thinking_signature;
    if (content.redacted) {
        dto.redacted = content.redacted;
    }
    return dto;
}

[[nodiscard]] inline ContentDto to_dto(const ImageContent& content) {
    ContentDto dto;
    dto.type = "image";
    dto.data = content.data;
    dto.mimeType = content.mime_type;
    return dto;
}

[[nodiscard]] inline ContentDto to_dto(const ToolCallContent& content) {
    ContentDto dto;
    dto.type = "toolCall";
    dto.id = content.id;
    dto.name = content.name;
    if (content.arguments) {
        dto.arguments = util::json_to_glaze(*content.arguments);
    }
    dto.rawArguments = content.raw_arguments;
    dto.thoughtSignature = content.thought_signature;
    if (!content.arguments_valid) {
        dto.argumentsValid = false;
    }
    dto.argumentError = content.argument_error;
    return dto;
}

[[nodiscard]] inline ContentDto to_dto(const Content& content) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, content);
}

[[nodiscard]] inline ContentDto to_dto(const AssistantContent& content) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, content);
}

[[nodiscard]] inline util::Expected<Content> content_from_dto(const ContentDto& dto, std::string_view context) {
    if (dto.type == "text") {
        if (auto required = require_field(dto.text, "text content", "text", context); !required) {
            return std::unexpected(required.error());
        }
        return Content{TextContent{*dto.text, dto.textSignature}};
    }
    if (dto.type == "thinking") {
        if (auto required = require_field(dto.thinking, "thinking content", "thinking", context); !required) {
            return std::unexpected(required.error());
        }
        return Content{ThinkingContent{*dto.thinking, dto.thinkingSignature, dto.redacted.value_or(false)}};
    }
    if (dto.type == "image") {
        if (auto required = require_field(dto.data, "image content", "data", context); !required) {
            return std::unexpected(required.error());
        }
        if (auto required = require_field(dto.mimeType, "image content", "mimeType", context); !required) {
            return std::unexpected(required.error());
        }
        return Content{ImageContent{*dto.data, *dto.mimeType}};
    }
    if (dto.type == "toolCall") {
        return std::unexpected(json_contract_error(
            "unexpected toolCall in non-assistant content",
            "toolCall content blocks are only valid in assistant messages",
            context));
    }

    return std::unexpected(json_contract_error(
        "unknown content discriminator",
        "unknown content type '" + dto.type + "'",
        context));
}

[[nodiscard]] inline util::Expected<std::vector<Content>> content_from_dto(
    const std::vector<ContentDto>& content,
    std::string_view context) {
    std::vector<Content> converted;
    converted.reserve(content.size());
    for (const auto& dto : content) {
        auto block = content_from_dto(dto, context);
        if (!block) {
            return std::unexpected(block.error());
        }
        converted.push_back(std::move(*block));
    }
    return converted;
}

[[nodiscard]] inline util::Expected<AssistantContent> assistant_content_from_dto(
    const ContentDto& dto,
    std::string_view context) {
    if (dto.type == "text") {
        if (auto required = require_field(dto.text, "text content", "text", context); !required) {
            return std::unexpected(required.error());
        }
        return AssistantContent{TextContent{*dto.text, dto.textSignature}};
    }
    if (dto.type == "thinking") {
        if (auto required = require_field(dto.thinking, "thinking content", "thinking", context); !required) {
            return std::unexpected(required.error());
        }
        return AssistantContent{ThinkingContent{*dto.thinking, dto.thinkingSignature, dto.redacted.value_or(false)}};
    }
    if (dto.type == "toolCall") {
        if (auto required = require_field(dto.id, "toolCall content", "id", context); !required) {
            return std::unexpected(required.error());
        }
        if (auto required = require_field(dto.name, "toolCall content", "name", context); !required) {
            return std::unexpected(required.error());
        }
        if (!dto.rawArguments && !dto.arguments) {
            return std::unexpected(json_contract_error(
                "missing required JSON field",
                "missing required field 'rawArguments' or 'arguments' for toolCall content",
                context));
        }
        auto raw_arguments = dto.rawArguments.value_or("");
        if (raw_arguments.empty() && dto.arguments) {
            auto raw = util::write_json(util::json_from_glaze(*dto.arguments));
            if (!raw) {
                return std::unexpected(raw.error());
            }
            raw_arguments = std::move(*raw);
        }
        return AssistantContent{ToolCallContent{
            *dto.id,
            *dto.name,
            dto.arguments ? std::optional<util::JsonValue>{util::json_from_glaze(*dto.arguments)} : std::nullopt,
            std::move(raw_arguments),
            dto.thoughtSignature,
            dto.argumentsValid.value_or(true),
            dto.argumentError,
        }};
    }
    if (dto.type == "image") {
        return std::unexpected(json_contract_error(
            "unexpected image in assistant content",
            "assistant messages cannot contain image content blocks",
            context));
    }

    return std::unexpected(json_contract_error(
        "unknown content discriminator",
        "unknown content type '" + dto.type + "'",
        context));
}

[[nodiscard]] inline util::Expected<std::vector<AssistantContent>> assistant_content_from_dto(
    const std::vector<ContentDto>& content,
    std::string_view context) {
    std::vector<AssistantContent> converted;
    converted.reserve(content.size());
    for (const auto& dto : content) {
        auto block = assistant_content_from_dto(dto, context);
        if (!block) {
            return std::unexpected(block.error());
        }
        converted.push_back(std::move(*block));
    }
    return converted;
}

[[nodiscard]] inline util::Expected<std::vector<AssistantContent>> required_assistant_content_from_dto(
    const std::optional<std::vector<ContentDto>>& content,
    std::string_view role,
    std::string_view context) {
    if (!content) {
        return std::unexpected(json_contract_error(
            "missing content",
            std::string(role) + " message is missing the 'content' field",
            context));
    }
    return assistant_content_from_dto(*content, context);
}

[[nodiscard]] inline std::vector<ContentDto> to_content_dtos(const std::vector<Content>& content) {
    std::vector<ContentDto> dtos;
    dtos.reserve(content.size());
    for (const auto& block : content) {
        dtos.push_back(to_dto(block));
    }
    return dtos;
}

[[nodiscard]] inline std::vector<ContentDto> to_assistant_content_dtos(
    const std::vector<AssistantContent>& content) {
    std::vector<ContentDto> dtos;
    dtos.reserve(content.size());
    for (const auto& block : content) {
        dtos.push_back(to_dto(block));
    }
    return dtos;
}

[[nodiscard]] inline DiagnosticErrorInfoDto to_dto(const DiagnosticErrorInfo& info) {
    DiagnosticErrorInfoDto dto;
    dto.name = info.name;
    dto.message = info.message;
    dto.stack = info.stack;
    dto.code = info.code;
    return dto;
}

[[nodiscard]] inline DiagnosticErrorInfo diagnostic_error_info_from_dto(
    const DiagnosticErrorInfoDto& dto) {
    DiagnosticErrorInfo info;
    info.name = dto.name;
    info.message = dto.message;
    info.stack = dto.stack;
    info.code = dto.code;
    return info;
}

[[nodiscard]] inline DiagnosticEntryDto to_dto(const DiagnosticEntry& entry) {
    DiagnosticEntryDto dto;
    dto.type = entry.type;
    dto.timestamp = entry.timestamp;
    if (entry.error) {
        dto.error = to_dto(*entry.error);
    }
    if (entry.details) {
        dto.details = util::json_to_glaze(*entry.details);
    }
    return dto;
}

[[nodiscard]] inline DiagnosticEntry diagnostic_entry_from_dto(
    const DiagnosticEntryDto& dto) {
    DiagnosticEntry entry;
    entry.type = dto.type;
    entry.timestamp = dto.timestamp;
    if (dto.error) {
        entry.error = diagnostic_error_info_from_dto(*dto.error);
    }
    if (dto.details) {
        entry.details = util::json_from_glaze(*dto.details);
    }
    return entry;
}

[[nodiscard]] inline std::vector<DiagnosticEntryDto> to_diagnostic_entry_dtos(
    const std::vector<DiagnosticEntry>& entries) {
    std::vector<DiagnosticEntryDto> dtos;
    dtos.reserve(entries.size());
    for (const auto& entry : entries) {
        dtos.push_back(to_dto(entry));
    }
    return dtos;
}

[[nodiscard]] inline util::Expected<std::vector<DiagnosticEntry>> diagnostic_entries_from_dto(
    const std::vector<DiagnosticEntryDto>& dtos,
    std::string_view /*context*/) {
    std::vector<DiagnosticEntry> entries;
    entries.reserve(dtos.size());
    for (const auto& dto : dtos) {
        entries.push_back(diagnostic_entry_from_dto(dto));
    }
    return entries;
}

[[nodiscard]] inline util::Expected<std::vector<DiagnosticEntry>> required_diagnostic_entries_from_dto(
    const std::optional<std::vector<DiagnosticEntryDto>>& dtos,
    std::string_view context) {
    if (!dtos) {
        return std::vector<DiagnosticEntry>{};
    }
    return diagnostic_entries_from_dto(*dtos, context);
}

[[nodiscard]] inline MessageDto to_dto(const SystemMessage& message) {
    MessageDto dto;
    dto.role = "system";
    dto.content = std::vector<ContentDto>{to_dto(TextContent{message.content, std::nullopt})};
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const UserMessage& message) {
    MessageDto dto;
    dto.role = "user";
    dto.content = to_content_dtos(message.content);
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const AssistantMessage& message) {
    MessageDto dto;
    dto.role = "assistant";
    dto.content = to_assistant_content_dtos(message.content);
    dto.api = message.api;
    dto.provider = message.provider;
    dto.model = message.model;
    dto.responseModel = message.response_model;
    dto.responseId = message.response_id;
    dto.usage = to_dto(message.usage);
    dto.stopReason = stop_reason_to_json(message.stop_reason);
    dto.errorMessage = message.error_message;
    if (message.diagnostics.has_value()) {
        dto.diagnostics = to_diagnostic_entry_dtos(*message.diagnostics);
    }
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const ToolResultMessage& message) {
    MessageDto dto;
    dto.role = "toolResult";
    dto.toolCallId = message.tool_call_id;
    dto.toolName = message.tool_name;
    dto.content = to_content_dtos(message.content);
    if (message.details) {
        dto.details = util::json_to_glaze(*message.details);
    }
    dto.isError = message.is_error;
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const BashExecutionMessage& message) {
    MessageDto dto;
    dto.role = "bashExecution";
    dto.command = message.command;
    dto.output = message.output;
    if (message.exit_code.has_value()) {
        dto.exitCode = static_cast<std::int64_t>(*message.exit_code);
    }
    dto.cancelled = message.cancelled;
    dto.truncated = message.truncated;
    dto.fullOutputPath = message.full_output_path;
    dto.excludeFromContext = message.exclude_from_context;
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const CustomMessage& message) {
    MessageDto dto;
    dto.role = "custom";
    dto.customType = message.custom_type;
    dto.content = to_content_dtos(message.content);
    dto.display = message.display;
    if (message.details) {
        dto.details = util::json_to_glaze(*message.details);
    }
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const BranchSummaryMessage& message) {
    MessageDto dto;
    dto.role = "branchSummary";
    dto.summary = message.summary;
    dto.fromId = message.from_id;
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const CompactionSummaryMessage& message) {
    MessageDto dto;
    dto.role = "compactionSummary";
    dto.summary = message.summary;
    dto.tokensBefore = message.tokens_before;
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const MessageVariant& message) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, message);
}

[[nodiscard]] inline util::Expected<MessageVariant> message_from_dto(const MessageDto& dto, std::string_view context) {
    if (dto.role == "system") {
        auto content = required_content_from_dto(dto.content, dto.role, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        std::string text;
        for (const auto& block : *content) {
            if (const auto* text_block = std::get_if<TextContent>(&block)) {
                text += text_block->text;
            }
        }
        return MessageVariant{SystemMessage{std::move(text), dto.timestamp}};
    }

    if (dto.role == "user") {
        auto content = required_content_from_dto(dto.content, dto.role, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        return MessageVariant{UserMessage{std::move(*content), dto.timestamp}};
    }

    if (dto.role == "assistant") {
        for (const auto& identity : {
                 std::pair{&dto.api, std::string_view{"api"}},
                 std::pair{&dto.provider, std::string_view{"provider"}},
                 std::pair{&dto.model, std::string_view{"model"}}}) {
            if (auto required = require_non_empty_string(
                    *identity.first, "assistant message", identity.second, context); !required) {
                return std::unexpected(required.error());
            }
        }
        if (dto.timestamp < kMinimumRealUnixEpochMilliseconds) {
            return std::unexpected(detail::json_contract_error(
                "invalid assistant timestamp",
                "required field 'timestamp' for assistant message must be a real Unix epoch millisecond value",
                context));
        }
        auto content = required_assistant_content_from_dto(dto.content, dto.role, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        AssistantMessage message;
        message.content = std::move(*content);
        message.api = *dto.api;
        message.provider = *dto.provider;
        message.model = *dto.model;
        message.response_model = dto.responseModel;
        message.response_id = dto.responseId;
        if (auto required = require_field(dto.usage, "assistant message", "usage", context); !required) {
            return std::unexpected(required.error());
        }
        auto usage = usage_from_dto(*dto.usage, context);
        if (!usage) {
            return std::unexpected(usage.error());
        }
        message.usage = std::move(*usage);
        if (auto required = require_field(dto.stopReason, "assistant message", "stopReason", context); !required) {
            return std::unexpected(required.error());
        }
        const auto stop_reason = stop_reason_from_json(*dto.stopReason);
        if (!stop_reason) {
            return std::unexpected(detail::json_contract_error(
                "unsupported assistant stop reason",
                "unsupported stopReason '" + *dto.stopReason + "' for assistant message",
                context));
        }
        message.stop_reason = *stop_reason;
        message.error_message = dto.errorMessage;
        if (dto.diagnostics.has_value()) {
            auto diags = required_diagnostic_entries_from_dto(dto.diagnostics, context);
            if (!diags) {
                return std::unexpected(diags.error());
            }
            message.diagnostics = std::move(*diags);
        }
        message.timestamp = dto.timestamp;
        return MessageVariant{std::move(message)};
    }

    if (dto.role == "toolResult") {
        auto content = required_content_from_dto(dto.content, dto.role, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        if (auto required = require_field(dto.toolCallId, "toolResult message", "toolCallId", context); !required) {
            return std::unexpected(required.error());
        }
        if (auto required = require_field(dto.toolName, "toolResult message", "toolName", context); !required) {
            return std::unexpected(required.error());
        }
        return MessageVariant{ToolResultMessage{
            *dto.toolCallId,
            *dto.toolName,
            std::move(*content),
            dto.details ? std::optional<util::JsonValue>{util::json_from_glaze(*dto.details)} : std::nullopt,
            dto.isError.value_or(false),
            dto.timestamp,
        }};
    }

    if (dto.role == "bashExecution") {
        if (auto required = require_field(dto.command, "bashExecution message", "command", context); !required) {
            return std::unexpected(required.error());
        }
        return MessageVariant{BashExecutionMessage{
            *dto.command,
            dto.output.value_or(""),
            dto.exitCode ? std::optional<int>{static_cast<int>(*dto.exitCode)} : std::nullopt,
            dto.cancelled.value_or(false),
            dto.truncated.value_or(false),
            dto.fullOutputPath,
            dto.excludeFromContext.value_or(false),
            dto.timestamp,
        }};
    }

    if (dto.role == "custom") {
        if (auto required = require_field(dto.customType, "custom message", "customType", context); !required) {
            return std::unexpected(required.error());
        }
        std::vector<Content> content;
        if (dto.content) {
            auto blocks = content_from_dto(*dto.content, context);
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            content = std::move(*blocks);
        }
        return MessageVariant{CustomMessage{
            *dto.customType,
            std::move(content),
            dto.display.value_or(true),
            dto.details ? std::optional<util::JsonValue>{util::json_from_glaze(*dto.details)} : std::nullopt,
            dto.timestamp,
        }};
    }

    if (dto.role == "branchSummary") {
        if (auto required = require_field(dto.summary, "branchSummary message", "summary", context); !required) {
            return std::unexpected(required.error());
        }
        if (auto required = require_field(dto.fromId, "branchSummary message", "fromId", context); !required) {
            return std::unexpected(required.error());
        }
        return MessageVariant{BranchSummaryMessage{
            *dto.summary,
            *dto.fromId,
            dto.timestamp,
        }};
    }

    if (dto.role == "compactionSummary") {
        if (auto required = require_field(dto.summary, "compactionSummary message", "summary", context); !required) {
            return std::unexpected(required.error());
        }
        return MessageVariant{CompactionSummaryMessage{
            *dto.summary,
            dto.tokensBefore.value_or(0),
            dto.timestamp,
        }};
    }

    return std::unexpected(json_contract_error(
        "unknown message role",
        "unknown message role '" + dto.role + "'",
        context));
}

[[nodiscard]] inline FunctionToolDto to_context_tool_dto(const Tool& tool) {
    return to_function_tool_dto(tool);
}

[[nodiscard]] inline util::Expected<Tool> tool_from_context_tool_dto(const FunctionToolDto& dto) {
    auto parameters = schema_from_tool_parameters_dto(dto.parameters);
    if (!parameters) {
        return std::unexpected(parameters.error());
    }
    return Tool{dto.name, dto.description, std::move(*parameters)};
}

[[nodiscard]] inline ContextDto to_dto(const AiContext& context) {
    ContextDto dto;
    dto.systemPrompt = context.system_prompt;
    if (!context.model.empty()) {
        dto.model = context.model;
    }
    for (const auto& message : context.messages) {
        dto.messages.push_back(to_dto(message));
    }
    if (!context.tools.empty()) {
        std::vector<FunctionToolDto> tools;
        for (const auto& tool : context.tools) {
            tools.push_back(to_context_tool_dto(tool));
        }
        dto.tools = std::move(tools);
    }
    return dto;
}

[[nodiscard]] inline util::Expected<AiContext> context_from_dto(const ContextDto& dto, std::string_view context_json) {
    AiContext context;
    context.system_prompt = dto.systemPrompt;
    context.model = dto.model.value_or("");
    for (const auto& message_dto : dto.messages) {
        auto message = message_from_dto(message_dto, context_json);
        if (!message) {
            return std::unexpected(message.error());
        }
        context.messages.push_back(std::move(*message));
    }
    if (dto.tools) {
        for (const auto& tool_dto : *dto.tools) {
            auto tool = tool_from_context_tool_dto(tool_dto);
            if (!tool) {
                return std::unexpected(tool.error());
            }
            context.tools.push_back(std::move(*tool));
        }
    }
    return context;
}

} // namespace detail

[[nodiscard]] inline MessageDto to_message_dto(const MessageVariant& message) {
    return detail::to_dto(message);
}

[[nodiscard]] inline util::Expected<MessageVariant> message_from_dto(const MessageDto& dto, std::string_view context = {}) {
    return detail::message_from_dto(dto, context);
}

[[nodiscard]] inline util::Expected<std::string> write_message_json(const MessageVariant& message) {
    return util::write_json(detail::to_dto(message));
}

[[nodiscard]] inline util::Expected<MessageVariant> read_message_json(std::string_view json) {
    auto dto = util::read_json<MessageDto>(json);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    return detail::message_from_dto(*dto, json);
}

[[nodiscard]] inline util::Expected<std::string> write_context_json(const AiContext& context) {
    return util::write_json(detail::to_dto(context));
}

[[nodiscard]] inline util::Expected<AiContext> read_context_json(std::string_view json) {
    auto dto = util::read_json<ContextDto>(json);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    return detail::context_from_dto(*dto, json);
}

} // namespace cch::ai::glaze
