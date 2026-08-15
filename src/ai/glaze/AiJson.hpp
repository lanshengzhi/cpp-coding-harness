#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Usage.hpp>
#include "ToolDtos.hpp"
#include "support/JsonGlaze.hpp"

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
    std::optional<std::string> text{std::nullopt};
    std::optional<std::string> textSignature{std::nullopt};
    std::optional<std::string> thinking{std::nullopt};
    std::optional<std::string> thinkingSignature{std::nullopt};
    std::optional<bool> redacted{std::nullopt};
    std::optional<std::string> data{std::nullopt};
    std::optional<std::string> mimeType{std::nullopt};
    std::optional<std::string> id{std::nullopt};
    std::optional<std::string> name{std::nullopt};
    std::optional<glz::generic> arguments{std::nullopt};
    std::optional<std::string> rawArguments{std::nullopt};
    std::optional<std::string> thoughtSignature{std::nullopt};
    std::optional<bool> argumentsValid{std::nullopt};
    std::optional<std::string> argumentError{std::nullopt};
};

/// Message `content` payload: pi `UserMessage.content` may carry either a
/// plain string or a block array; every other role carries a block array.
using MessageContentDto = std::variant<std::string, std::vector<ContentDto>>;

struct MessageDto {
    std::string role;
    std::optional<MessageContentDto> content{std::nullopt};
    std::optional<std::string> api{std::nullopt};
    std::optional<std::string> provider{std::nullopt};
    std::optional<std::string> model{std::nullopt};
    std::optional<std::string> responseModel{std::nullopt};
    std::optional<std::string> responseId{std::nullopt};
    std::optional<UsageDto> usage{std::nullopt};
    std::optional<std::string> stopReason{std::nullopt};
    std::optional<std::string> rawStopReason{std::nullopt};
    std::optional<std::string> errorMessage{std::nullopt};
    std::optional<std::vector<DiagnosticEntryDto>> diagnostics{std::nullopt};
    std::optional<std::string> toolCallId{std::nullopt};
    std::optional<std::string> toolName{std::nullopt};
    std::optional<glz::generic> details{std::nullopt};
    std::optional<bool> isError{std::nullopt};
    // Extended message type fields
    std::optional<std::string> command{std::nullopt};
    std::optional<std::string> output{std::nullopt};
    std::optional<std::int64_t> exitCode{std::nullopt};
    std::optional<bool> cancelled{std::nullopt};
    std::optional<bool> truncated{std::nullopt};
    std::optional<std::string> fullOutputPath{std::nullopt};
    std::optional<bool> excludeFromContext{std::nullopt};
    std::optional<std::string> customType{std::nullopt};
    std::optional<bool> display{std::nullopt};
    std::optional<std::string> summary{std::nullopt};
    std::optional<std::string> fromId{std::nullopt};
    std::optional<std::int64_t> tokensBefore{std::nullopt};
    std::int64_t timestamp{};
};

struct ContextDto {
    std::optional<std::string> systemPrompt;
    std::vector<MessageDto> messages;
    std::optional<std::vector<FunctionToolDto>> tools;
};

[[nodiscard]] inline std::string stop_reason_to_json(AssistantStopReason reason) {
    return stop_reason_to_string(reason);
}

[[nodiscard]] inline std::optional<AssistantStopReason> stop_reason_from_json(std::string_view reason) {
    if (reason == "pending") {
        return AssistantStopReason::Pending;
    }
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

[[nodiscard]] inline support::Error json_contract_error(std::string message, std::string detail, std::string_view context) {
    return support::make_error(
        support::ErrorCode::JsonParse,
        std::move(message),
        std::move(detail),
        context.empty() ? std::nullopt : std::optional<std::string>{std::string(context)});
}

[[nodiscard]] inline support::Expected<std::vector<Content>> content_from_dto(
    const std::vector<ContentDto>& content,
    std::string_view context);

template <typename T>
[[nodiscard]] inline support::ExpectedVoid require_field(
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

[[nodiscard]] inline support::ExpectedVoid require_non_empty_string(
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

[[nodiscard]] inline support::Expected<std::vector<Content>> required_content_from_dto(
    const std::optional<MessageContentDto>& content,
    std::string_view role,
    std::string_view context) {
    if (!content) {
        return std::unexpected(json_contract_error(
            "missing required JSON field",
            std::string{"missing required field 'content' for "} + std::string(role),
            context));
    }
    if (const auto* text = std::get_if<std::string>(&*content)) {
        return std::unexpected(json_contract_error(
            "content must be a block array",
            std::string{"content for "} + std::string(role) +
                " must be a JSON array, found a JSON string",
            context));
    }
    return content_from_dto(std::get<std::vector<ContentDto>>(*content), context);
}

/// User messages accept both alternatives and preserve the caller's choice:
/// a JSON string loads as the string alternative, a JSON array as the block
/// array. Never canonicalizes one into the other.
[[nodiscard]] inline support::Expected<std::variant<std::string, std::vector<Content>>> user_content_from_dto(
    const std::optional<MessageContentDto>& content,
    std::string_view context) {
    if (!content) {
        return std::unexpected(json_contract_error(
            "missing required JSON field",
            "missing required field 'content' for user message",
            context));
    }
    if (const auto* text = std::get_if<std::string>(&*content)) {
        return std::variant<std::string, std::vector<Content>>{*text};
    }
    auto blocks = content_from_dto(std::get<std::vector<ContentDto>>(*content), context);
    if (!blocks) {
        return std::unexpected(blocks.error());
    }
    return std::variant<std::string, std::vector<Content>>{std::move(*blocks)};
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

[[nodiscard]] inline support::Expected<Usage> usage_from_dto(
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
    return ContentDto{
        .type = "text",
        .text = content.text,
        .textSignature = content.text_signature,
    };
}

[[nodiscard]] inline ContentDto to_dto(const ThinkingContent& content) {
    return ContentDto{
        .type = "thinking",
        .thinking = content.thinking,
        .thinkingSignature = content.thinking_signature,
        .redacted = content.redacted ? std::optional<bool>{true} : std::nullopt,
    };
}

[[nodiscard]] inline ContentDto to_dto(const ImageContent& content) {
    return ContentDto{
        .type = "image",
        .data = content.data,
        .mimeType = content.mime_type,
    };
}

[[nodiscard]] inline ContentDto to_dto(const ToolCallContent& content) {
    return ContentDto{
        .type = "toolCall",
        .id = content.id,
        .name = content.name,
        .arguments = content.arguments
            ? std::optional<glz::generic>{support::json_to_glaze(*content.arguments)}
            : std::nullopt,
        .rawArguments = content.raw_arguments,
        .thoughtSignature = content.thought_signature,
        .argumentsValid = content.arguments_valid ? std::nullopt : std::optional<bool>{false},
        .argumentError = content.argument_error,
    };
}

[[nodiscard]] inline ContentDto to_dto(const Content& content) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, content);
}

[[nodiscard]] inline ContentDto to_dto(const AssistantContent& content) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, content);
}

[[nodiscard]] inline support::Expected<Content> content_from_dto(const ContentDto& dto, std::string_view context) {
    if (dto.type == "text") {
        if (auto required = require_field(dto.text, "text content", "text", context); !required) {
            return std::unexpected(required.error());
        }
        return Content{TextContent{
            .text = *dto.text,
            .text_signature = dto.textSignature,
        }};
    }
    if (dto.type == "thinking") {
        if (auto required = require_field(dto.thinking, "thinking content", "thinking", context); !required) {
            return std::unexpected(required.error());
        }
        return Content{ThinkingContent{
            .thinking = *dto.thinking,
            .thinking_signature = dto.thinkingSignature,
            .redacted = dto.redacted.value_or(false),
        }};
    }
    if (dto.type == "image") {
        if (auto required = require_field(dto.data, "image content", "data", context); !required) {
            return std::unexpected(required.error());
        }
        if (auto required = require_field(dto.mimeType, "image content", "mimeType", context); !required) {
            return std::unexpected(required.error());
        }
        return Content{ImageContent{
            .data = *dto.data,
            .mime_type = *dto.mimeType,
        }};
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

[[nodiscard]] inline support::Expected<std::vector<Content>> content_from_dto(
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

[[nodiscard]] inline support::Expected<AssistantContent> assistant_content_from_dto(
    const ContentDto& dto,
    std::string_view context) {
    if (dto.type == "text") {
        if (auto required = require_field(dto.text, "text content", "text", context); !required) {
            return std::unexpected(required.error());
        }
        return AssistantContent{TextContent{
            .text = *dto.text,
            .text_signature = dto.textSignature,
        }};
    }
    if (dto.type == "thinking") {
        if (auto required = require_field(dto.thinking, "thinking content", "thinking", context); !required) {
            return std::unexpected(required.error());
        }
        return AssistantContent{ThinkingContent{
            .thinking = *dto.thinking,
            .thinking_signature = dto.thinkingSignature,
            .redacted = dto.redacted.value_or(false),
        }};
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
            auto raw = support::write_json(support::json_from_glaze(*dto.arguments));
            if (!raw) {
                return std::unexpected(raw.error());
            }
            raw_arguments = std::move(*raw);
        }
        return AssistantContent{ToolCallContent{
            .id = *dto.id,
            .name = *dto.name,
            .arguments = dto.arguments
                ? std::optional<support::JsonValue>{support::json_from_glaze(*dto.arguments)}
                : std::nullopt,
            .raw_arguments = std::move(raw_arguments),
            .thought_signature = dto.thoughtSignature,
            .arguments_valid = dto.argumentsValid.value_or(true),
            .argument_error = dto.argumentError,
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

[[nodiscard]] inline support::Expected<std::vector<AssistantContent>> assistant_content_from_dto(
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

[[nodiscard]] inline support::Expected<std::vector<AssistantContent>> required_assistant_content_from_dto(
    const std::optional<MessageContentDto>& content,
    std::string_view role,
    std::string_view context) {
    if (!content) {
        return std::unexpected(json_contract_error(
            "missing content",
            std::string(role) + " message is missing the 'content' field",
            context));
    }
    if (const auto* text = std::get_if<std::string>(&*content)) {
        return std::unexpected(json_contract_error(
            "content must be a block array",
            std::string(role) +
                " message content must be a JSON array, found a JSON string",
            context));
    }
    return assistant_content_from_dto(std::get<std::vector<ContentDto>>(*content), context);
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
        dto.details = support::json_to_glaze(*entry.details);
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
        entry.details = support::json_from_glaze(*dto.details);
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

[[nodiscard]] inline support::Expected<std::vector<DiagnosticEntry>> diagnostic_entries_from_dto(
    const std::vector<DiagnosticEntryDto>& dtos,
    std::string_view /*context*/) {
    std::vector<DiagnosticEntry> entries;
    entries.reserve(dtos.size());
    for (const auto& dto : dtos) {
        entries.push_back(diagnostic_entry_from_dto(dto));
    }
    return entries;
}

[[nodiscard]] inline support::Expected<std::vector<DiagnosticEntry>> required_diagnostic_entries_from_dto(
    const std::optional<std::vector<DiagnosticEntryDto>>& dtos,
    std::string_view context) {
    if (!dtos) {
        return std::vector<DiagnosticEntry>{};
    }
    return diagnostic_entries_from_dto(*dtos, context);
}

[[nodiscard]] inline MessageDto to_dto(const SystemMessage& message) {
    return MessageDto{
        .role = "system",
        .content = std::vector<ContentDto>{to_dto(TextContent{
            .text = message.content,
            .text_signature = std::nullopt,
        })},
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline std::optional<MessageContentDto> to_dto_content(const UserMessage& message) {
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        return MessageContentDto{*text};
    }
    return MessageContentDto{to_content_dtos(std::get<std::vector<Content>>(message.content))};
}

[[nodiscard]] inline MessageDto to_dto(const UserMessage& message) {
    return MessageDto{
        .role = "user",
        .content = to_dto_content(message),
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const AssistantMessage& message) {
    return MessageDto{
        .role = "assistant",
        .content = to_assistant_content_dtos(message.content),
        .api = message.api,
        .provider = message.provider,
        .model = message.model,
        .responseModel = message.response_model,
        .responseId = message.response_id,
        .usage = to_dto(message.usage),
        .stopReason = stop_reason_to_json(message.stop_reason),
        .rawStopReason = message.raw_stop_reason,
        .errorMessage = message.error_message,
        .diagnostics = message.diagnostics
            ? std::optional<std::vector<DiagnosticEntryDto>>{
                  to_diagnostic_entry_dtos(*message.diagnostics)}
            : std::nullopt,
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const ToolResultMessage& message) {
    return MessageDto{
        .role = "toolResult",
        .content = to_content_dtos(message.content),
        .toolCallId = message.tool_call_id,
        .toolName = message.tool_name,
        .details = message.details
            ? std::optional<glz::generic>{support::json_to_glaze(*message.details)}
            : std::nullopt,
        .isError = message.is_error,
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const BashExecutionMessage& message) {
    return MessageDto{
        .role = "bashExecution",
        .command = message.command,
        .output = message.output,
        .exitCode = message.exit_code
            ? std::optional<std::int64_t>{static_cast<std::int64_t>(*message.exit_code)}
            : std::nullopt,
        .cancelled = message.cancelled,
        .truncated = message.truncated,
        .fullOutputPath = message.full_output_path,
        .excludeFromContext = message.exclude_from_context,
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const CustomMessage& message) {
    return MessageDto{
        .role = "custom",
        .content = to_content_dtos(message.content),
        .details = message.details
            ? std::optional<glz::generic>{support::json_to_glaze(*message.details)}
            : std::nullopt,
        .customType = message.custom_type,
        .display = message.display,
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const BranchSummaryMessage& message) {
    return MessageDto{
        .role = "branchSummary",
        .summary = message.summary,
        .fromId = message.from_id,
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const CompactionSummaryMessage& message) {
    return MessageDto{
        .role = "compactionSummary",
        .summary = message.summary,
        .tokensBefore = message.tokens_before,
        .timestamp = message.timestamp,
    };
}

[[nodiscard]] inline MessageDto to_dto(const MessageVariant& message) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, message);
}

[[nodiscard]] inline support::Expected<MessageVariant> message_from_dto(const MessageDto& dto, std::string_view context) {
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
        return MessageVariant{SystemMessage{
            .content = std::move(text),
            .timestamp = dto.timestamp,
        }};
    }

    if (dto.role == "user") {
        auto content = user_content_from_dto(dto.content, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        return MessageVariant{UserMessage{
            .content = std::move(*content),
            .timestamp = dto.timestamp,
        }};
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
        if (auto required = require_field(dto.usage, "assistant message", "usage", context); !required) {
            return std::unexpected(required.error());
        }
        auto usage = usage_from_dto(*dto.usage, context);
        if (!usage) {
            return std::unexpected(usage.error());
        }
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
        std::optional<std::vector<DiagnosticEntry>> diagnostics;
        if (dto.diagnostics.has_value()) {
            auto diags = required_diagnostic_entries_from_dto(dto.diagnostics, context);
            if (!diags) {
                return std::unexpected(diags.error());
            }
            diagnostics = std::move(*diags);
        }
        return MessageVariant{AssistantMessage{
            .content = std::move(*content),
            .api = *dto.api,
            .provider = *dto.provider,
            .model = *dto.model,
            .response_model = dto.responseModel,
            .response_id = dto.responseId,
            .usage = std::move(*usage),
            .stop_reason = *stop_reason,
            .raw_stop_reason = dto.rawStopReason,
            .error_message = dto.errorMessage,
            .diagnostics = std::move(diagnostics),
            .timestamp = dto.timestamp,
        }};
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
            .tool_call_id = *dto.toolCallId,
            .tool_name = *dto.toolName,
            .content = std::move(*content),
            .details = dto.details
                ? std::optional<support::JsonValue>{support::json_from_glaze(*dto.details)}
                : std::nullopt,
            .is_error = dto.isError.value_or(false),
            .timestamp = dto.timestamp,
        }};
    }

    if (dto.role == "bashExecution") {
        if (auto required = require_field(dto.command, "bashExecution message", "command", context); !required) {
            return std::unexpected(required.error());
        }
        return MessageVariant{BashExecutionMessage{
            .command = *dto.command,
            .output = dto.output.value_or(""),
            .exit_code = dto.exitCode
                ? std::optional<int>{static_cast<int>(*dto.exitCode)}
                : std::nullopt,
            .cancelled = dto.cancelled.value_or(false),
            .truncated = dto.truncated.value_or(false),
            .full_output_path = dto.fullOutputPath,
            .exclude_from_context = dto.excludeFromContext.value_or(false),
            .timestamp = dto.timestamp,
        }};
    }

    if (dto.role == "custom") {
        if (auto required = require_field(dto.customType, "custom message", "customType", context); !required) {
            return std::unexpected(required.error());
        }
        std::vector<Content> content;
        if (dto.content) {
            auto blocks = required_content_from_dto(dto.content, "custom message", context);
            if (!blocks) {
                return std::unexpected(blocks.error());
            }
            content = std::move(*blocks);
        }
        return MessageVariant{CustomMessage{
            .custom_type = *dto.customType,
            .content = std::move(content),
            .display = dto.display.value_or(true),
            .details = dto.details
                ? std::optional<support::JsonValue>{support::json_from_glaze(*dto.details)}
                : std::nullopt,
            .timestamp = dto.timestamp,
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
            .summary = *dto.summary,
            .from_id = *dto.fromId,
            .timestamp = dto.timestamp,
        }};
    }

    if (dto.role == "compactionSummary") {
        if (auto required = require_field(dto.summary, "compactionSummary message", "summary", context); !required) {
            return std::unexpected(required.error());
        }
        return MessageVariant{CompactionSummaryMessage{
            .summary = *dto.summary,
            .tokens_before = dto.tokensBefore.value_or(0),
            .timestamp = dto.timestamp,
        }};
    }

    return std::unexpected(json_contract_error(
        "unknown message role",
        "unknown message role '" + dto.role + "'",
        context));
}

[[nodiscard]] inline ContextDto to_dto(const AiContext& context) {
    ContextDto dto;
    dto.systemPrompt = context.system_prompt;
    for (const auto& message : context.messages) {
        dto.messages.push_back(to_dto(message));
    }
    if (!context.tools.empty()) {
        std::vector<FunctionToolDto> tools;
        for (const auto& tool : context.tools) {
            tools.push_back(to_function_tool_dto(tool));
        }
        dto.tools = std::move(tools);
    }
    return dto;
}

[[nodiscard]] inline support::Expected<AiContext> context_from_dto(const ContextDto& dto, std::string_view context_json) {
    AiContext context;
    context.system_prompt = dto.systemPrompt;
    for (const auto& message_dto : dto.messages) {
        auto message = message_from_dto(message_dto, context_json);
        if (!message) {
            return std::unexpected(message.error());
        }
        context.messages.push_back(std::move(*message));
    }
    if (dto.tools) {
        for (const auto& tool_dto : *dto.tools) {
            context.tools.push_back(tool_from_function_tool_dto(tool_dto));
        }
    }
    return context;
}

} // namespace detail

[[nodiscard]] inline MessageDto to_message_dto(const MessageVariant& message) {
    return detail::to_dto(message);
}

[[nodiscard]] inline support::Expected<MessageVariant> message_from_dto(const MessageDto& dto, std::string_view context = {}) {
    return detail::message_from_dto(dto, context);
}

[[nodiscard]] inline support::Expected<std::string> write_message_json(const MessageVariant& message) {
    return support::write_json(detail::to_dto(message));
}

[[nodiscard]] inline support::Expected<MessageVariant> read_message_json(std::string_view json) {
    auto dto = support::read_json<MessageDto>(json);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    return detail::message_from_dto(*dto, json);
}

[[nodiscard]] inline support::Expected<std::string> write_context_json(const AiContext& context) {
    return support::write_json(detail::to_dto(context));
}

[[nodiscard]] inline support::Expected<AiContext> read_context_json(std::string_view json) {
    auto dto = support::read_json<ContextDto>(json);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    return detail::context_from_dto(*dto, json);
}

} // namespace cch::ai::glaze
