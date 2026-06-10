#pragma once

#include "../Context.hpp"
#include "../Message.hpp"
#include "../Usage.hpp"
#include "ToolSchemaDtos.hpp"
#include "../../util/Error.hpp"

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
    double input{};
    double output{};
    double cacheRead{};
    double cacheWrite{};
    double total{};
};

struct UsageDto {
    std::int64_t input{};
    std::int64_t output{};
    std::int64_t cacheRead{};
    std::int64_t cacheWrite{};
    std::int64_t totalTokens{};
    UsageCostDto cost{};
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
    std::vector<ContentDto> content;
    std::optional<std::string> api;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> responseModel;
    std::optional<std::string> responseId;
    std::optional<UsageDto> usage;
    std::optional<std::string> stopReason;
    std::optional<std::string> errorMessage;
    std::optional<std::string> toolCallId;
    std::optional<std::string> toolName;
    std::optional<glz::generic> details;
    std::optional<bool> isError;
    std::int64_t timestamp{};
};

struct ContextDto {
    std::optional<std::string> systemPrompt;
    std::vector<MessageDto> messages;
    std::optional<std::vector<FunctionToolDto>> tools;
};

namespace detail {

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

[[nodiscard]] inline UsageDto to_dto(const Usage& usage) {
    return UsageDto{
        usage.input,
        usage.output,
        usage.cache_read,
        usage.cache_write,
        usage.total_tokens,
        UsageCostDto{
            usage.cost.input,
            usage.cost.output,
            usage.cost.cache_read,
            usage.cost.cache_write,
            usage.cost.total,
        },
    };
}

[[nodiscard]] inline Usage usage_from_dto(const UsageDto& dto) {
    return Usage{
        dto.input,
        dto.output,
        dto.cacheRead,
        dto.cacheWrite,
        dto.totalTokens,
        UsageCost{
            dto.cost.input,
            dto.cost.output,
            dto.cost.cacheRead,
            dto.cost.cacheWrite,
            dto.cost.total,
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
    dto.arguments = content.arguments;
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

[[nodiscard]] inline util::Expected<Content> content_from_dto(const ContentDto& dto, std::string_view context) {
    if (dto.type == "text") {
        return Content{TextContent{dto.text.value_or(""), dto.textSignature}};
    }
    if (dto.type == "thinking") {
        return Content{ThinkingContent{dto.thinking.value_or(""), dto.thinkingSignature, dto.redacted.value_or(false)}};
    }
    if (dto.type == "image") {
        return Content{ImageContent{dto.data.value_or(""), dto.mimeType.value_or("")}};
    }
    if (dto.type == "toolCall") {
        auto raw_arguments = dto.rawArguments.value_or("");
        if (raw_arguments.empty() && dto.arguments) {
            auto raw = util::write_json(*dto.arguments);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            raw_arguments = std::move(*raw);
        }
        return Content{ToolCallContent{
            dto.id.value_or(""),
            dto.name.value_or(""),
            dto.arguments,
            std::move(raw_arguments),
            dto.thoughtSignature,
            dto.argumentsValid.value_or(true),
            dto.argumentError,
        }};
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

[[nodiscard]] inline std::vector<ContentDto> to_content_dtos(const std::vector<Content>& content) {
    std::vector<ContentDto> dtos;
    dtos.reserve(content.size());
    for (const auto& block : content) {
        dtos.push_back(to_dto(block));
    }
    return dtos;
}

[[nodiscard]] inline MessageDto to_dto(const SystemMessage& message) {
    MessageDto dto;
    dto.role = "system";
    dto.content.push_back(to_dto(TextContent{message.content, std::nullopt}));
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
    dto.content = to_content_dtos(message.content);
    if (!message.api.empty()) {
        dto.api = message.api;
    }
    if (!message.provider.empty()) {
        dto.provider = message.provider;
    }
    if (!message.model.empty()) {
        dto.model = message.model;
    }
    dto.responseModel = message.response_model;
    dto.responseId = message.response_id;
    if (message.usage) {
        dto.usage = to_dto(*message.usage);
    }
    dto.stopReason = stop_reason_to_json(message.stop_reason);
    dto.errorMessage = message.error_message;
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const ToolResultMessage& message) {
    MessageDto dto;
    dto.role = "toolResult";
    dto.toolCallId = message.tool_call_id;
    dto.toolName = message.tool_name;
    dto.content = to_content_dtos(message.content);
    dto.details = message.details;
    dto.isError = message.is_error;
    dto.timestamp = message.timestamp;
    return dto;
}

[[nodiscard]] inline MessageDto to_dto(const MessageVariant& message) {
    return std::visit([](const auto& concrete) { return to_dto(concrete); }, message);
}

[[nodiscard]] inline util::Expected<MessageVariant> message_from_dto(const MessageDto& dto, std::string_view context) {
    if (dto.role == "system") {
        auto content = content_from_dto(dto.content, context);
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
        auto content = content_from_dto(dto.content, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        return MessageVariant{UserMessage{std::move(*content), dto.timestamp}};
    }

    if (dto.role == "assistant") {
        auto content = content_from_dto(dto.content, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        AssistantMessage message;
        message.content = std::move(*content);
        message.api = dto.api.value_or("");
        message.provider = dto.provider.value_or("");
        message.model = dto.model.value_or("");
        message.response_model = dto.responseModel;
        message.response_id = dto.responseId;
        if (dto.usage) {
            message.usage = usage_from_dto(*dto.usage);
        }
        message.stop_reason = dto.stopReason ? stop_reason_from_json(*dto.stopReason) : AssistantStopReason::Unknown;
        message.error_message = dto.errorMessage;
        message.timestamp = dto.timestamp;
        return MessageVariant{std::move(message)};
    }

    if (dto.role == "toolResult") {
        auto content = content_from_dto(dto.content, context);
        if (!content) {
            return std::unexpected(content.error());
        }
        return MessageVariant{ToolResultMessage{
            dto.toolCallId.value_or(""),
            dto.toolName.value_or(""),
            std::move(*content),
            dto.details,
            dto.isError.value_or(false),
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
