#include <cch/ai/providers/OpenAIChatClient.hpp>

#include <cch/ai/providers/OpenAICompletionsCompat.hpp>
#include "ai/glaze/AiJson.hpp"
#include "ai/glaze/ProviderDtos.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace cch::ai::providers {
namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

struct ToolCallAccumulator {
    std::string id;
    std::string name;
    std::string raw_arguments;
    std::size_t content_index{};
};

void append_plain_text_part(std::string& text, std::string_view part, std::string_view separator) {
    if (part.empty()) {
        return;
    }
    if (!text.empty()) {
        text += separator;
    }
    text += part;
}

// Plain-text stand-in for the content blocks that carry model-readable text:
// text blocks map to their text, thinking blocks to a fixed omission marker,
// and images have no text form. The visit keeps the single mapping
// compile-time exhaustive over the Content alternatives.
[[nodiscard]] std::optional<std::string_view> text_like_content(const ai::Content& block) {
    return std::visit(
        Overloaded{
            [](const ai::TextContent& text) -> std::optional<std::string_view> {
                return text.text;
            },
            [](const ai::ThinkingContent&) -> std::optional<std::string_view> {
                return "[thinking content omitted]";
            },
            [](const ai::ImageContent&) -> std::optional<std::string_view> {
                return std::nullopt;
            },
        },
        block);
}

[[nodiscard]] std::string content_text(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto part = text_like_content(block)) {
            append_plain_text_part(text, *part, "\n");
        }
    }
    return text;
}

[[nodiscard]] bool has_image_content(const std::vector<ai::Content>& content) {
    return std::ranges::any_of(content, [](const ai::Content& block) {
        return std::holds_alternative<ai::ImageContent>(block);
    });
}

[[nodiscard]] ai::glaze::OpenAIContentPartDto text_part(std::string text) {
    return ai::glaze::OpenAIContentPartDto{
        .type = "text",
        .text = std::move(text),
        .image_url = std::nullopt,
    };
}

[[nodiscard]] ai::glaze::OpenAIContentPartDto image_part(const ai::ImageContent& image) {
    return ai::glaze::OpenAIContentPartDto{
        .type = "image_url",
        .text = std::nullopt,
        .image_url = ai::glaze::OpenAIImageUrlDto{
            .url = "data:" + image.mime_type + ";base64," + image.data,
        },
    };
}

[[nodiscard]] ai::glaze::OpenAIMessageContentDto user_content(
    const std::vector<ai::Content>& content) {
    if (!has_image_content(content)) {
        return content_text(content);
    }

    std::vector<ai::glaze::OpenAIContentPartDto> parts;
    parts.reserve(content.size());
    for (const auto& block : content) {
        if (const auto part = text_like_content(block)) {
            parts.push_back(text_part(std::string{*part}));
            continue;
        }
        // Only image blocks have no text form per the exhaustive mapping
        // above; probe instead of std::get so an unexpected alternative is
        // skipped rather than throwing bad_variant_access.
        if (const auto* image = std::get_if<ai::ImageContent>(&block)) {
            parts.push_back(image_part(*image));
        }
    }
    return parts;
}

[[nodiscard]] std::string tool_result_text(const std::vector<ai::Content>& content) {
    std::string text;
    bool saw_text_block = false;
    for (const auto& block : content) {
        const auto* text_block = std::get_if<ai::TextContent>(&block);
        if (text_block == nullptr) {
            continue;
        }
        if (saw_text_block) {
            text += '\n';
        }
        text += text_block->text;
        saw_text_block = true;
    }
    return text;
}

[[nodiscard]] std::string assistant_content_text(
    const std::vector<ai::AssistantContent>& content,
    const OpenAICompletionsCompat& compat) {
    std::string text;
    if (!compat.requires_thinking_as_text.value_or(false)) {
        for (const auto& block : content) {
            if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
                text += text_block->text;
            }
        }
        return text;
    }

    for (const auto& block : content) {
        if (const auto* thinking_block = std::get_if<ai::ThinkingContent>(&block)) {
            append_plain_text_part(text, thinking_block->thinking, "\n\n");
        } else if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
            append_plain_text_part(text, text_block->text, "\n\n");
        }
    }
    return text;
}

[[nodiscard]] std::string system_role(const OpenAICompletionsCompat& compat) {
    return compat.supports_developer_role.value_or(false) ? "developer" : "system";
}

[[nodiscard]] std::vector<ai::glaze::ProviderToolCallDto> tool_calls_from_assistant_content(
    const std::vector<ai::AssistantContent>& content) {
    std::vector<ai::glaze::ProviderToolCallDto> calls;
    for (const auto& block : content) {
        if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
            calls.push_back(ai::glaze::ProviderToolCallDto{
                .id = call->id,
                .type = "function",
                .function = ai::glaze::ProviderToolCallFunctionDto{
                    .name = call->name,
                    .arguments = call->raw_arguments,
                },
            });
        }
    }
    return calls;
}

void append_tool_result_assistant_bridge(
    std::vector<ai::glaze::OpenAIChatMessageDto>& messages) {
    messages.push_back(ai::glaze::OpenAIChatMessageDto{
        .role = "assistant",
        .content = "I have processed the tool results.",
    });
}

[[nodiscard]] ai::glaze::OpenAIChatMessageDto tool_result_to_openai(
    const ai::ToolResultMessage& tool,
    const OpenAICompletionsCompat& compat) {
    auto text = tool_result_text(tool.content);
    if (text.empty()) {
        text = has_image_content(tool.content)
            ? "(see attached image)"
            : "(no tool output)";
    }
    auto dto = ai::glaze::OpenAIChatMessageDto{
        .role = "tool",
        .content = std::move(text),
        .tool_call_id = tool.tool_call_id,
    };
    if (compat.requires_tool_result_name.value_or(false)) {
        dto.name = tool.tool_name;
    }
    return dto;
}

[[nodiscard]] ai::glaze::OpenAIChatMessageDto message_to_openai(
    const ai::MessageVariant& message,
    const OpenAICompletionsCompat& compat = {}) {
    return std::visit(
        Overloaded{
            [&compat](const ai::SystemMessage& system) {
                return ai::glaze::OpenAIChatMessageDto{
                    .role = system_role(compat),
                    .content = system.content,
                };
            },
            [](const ai::UserMessage& user) {
                return ai::glaze::OpenAIChatMessageDto{
                    .role = "user",
                    .content = user_content(user.content),
                };
            },
            [&compat](const ai::AssistantMessage& assistant) {
                auto calls = tool_calls_from_assistant_content(assistant.content);
                std::optional<std::vector<ai::glaze::ProviderToolCallDto>> tool_calls;
                if (!calls.empty()) {
                    tool_calls = std::move(calls);
                }
                return ai::glaze::OpenAIChatMessageDto{
                    .role = "assistant",
                    .content = assistant_content_text(assistant.content, compat),
                    .tool_calls = std::move(tool_calls),
                };
            },
            [&compat](const ai::ToolResultMessage& tool) {
                return tool_result_to_openai(tool, compat);
            },
            [](const ai::BashExecutionMessage& bash) {
                auto msg = bash_execution_to_user_message(bash);
                return ai::glaze::OpenAIChatMessageDto{
                    .role = "user",
                    .content = content_text(msg.content),
                };
            },
            [](const ai::CustomMessage& custom) {
                auto msg = custom_message_to_user_message(custom);
                return ai::glaze::OpenAIChatMessageDto{
                    .role = "user",
                    .content = user_content(msg.content),
                };
            },
            [](const ai::BranchSummaryMessage& branch) {
                auto msg = branch_summary_to_user_message(branch);
                return ai::glaze::OpenAIChatMessageDto{
                    .role = "user",
                    .content = content_text(msg.content),
                };
            },
            [](const ai::CompactionSummaryMessage& compaction) {
                auto msg = compaction_summary_to_user_message(compaction);
                return ai::glaze::OpenAIChatMessageDto{
                    .role = "user",
                    .content = content_text(msg.content),
                };
            },
        },
        message);
}

[[nodiscard]] bool is_valid_utf8(std::string_view value) {
    return value.empty() || glz::validate_utf8(value.data(), value.size());
}

[[nodiscard]] bool optional_has_valid_utf8(const std::optional<std::string>& value) {
    return !value || is_valid_utf8(*value);
}

[[nodiscard]] bool generic_has_valid_utf8(const glz::generic& value) {
    if (value.is_string()) {
        return is_valid_utf8(value.get_string());
    }
    if (value.is_array()) {
        for (const auto& entry : value.get_array()) {
            if (!generic_has_valid_utf8(entry)) {
                return false;
            }
        }
        return true;
    }
    if (value.is_object()) {
        for (const auto& [key, entry] : value.get_object()) {
            if (!is_valid_utf8(key) || !generic_has_valid_utf8(entry)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool tool_call_has_valid_utf8(const ai::glaze::ProviderToolCallDto& tool_call) {
    return is_valid_utf8(tool_call.id) &&
           is_valid_utf8(tool_call.type) &&
           is_valid_utf8(tool_call.function.name) &&
           is_valid_utf8(tool_call.function.arguments);
}

[[nodiscard]] bool content_has_valid_utf8(
    const ai::glaze::OpenAIMessageContentDto& content) {
    if (const auto* text = std::get_if<std::string>(&content)) {
        return is_valid_utf8(*text);
    }
    for (const auto& part : std::get<std::vector<ai::glaze::OpenAIContentPartDto>>(content)) {
        if (!is_valid_utf8(part.type) ||
            !optional_has_valid_utf8(part.text) ||
            (part.image_url && !is_valid_utf8(part.image_url->url))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool message_has_valid_utf8(const ai::glaze::OpenAIChatMessageDto& message) {
    if (!is_valid_utf8(message.role) ||
        !content_has_valid_utf8(message.content) ||
        !optional_has_valid_utf8(message.name) ||
        !optional_has_valid_utf8(message.tool_call_id)) {
        return false;
    }
    if (message.tool_calls) {
        for (const auto& tool_call : *message.tool_calls) {
            if (!tool_call_has_valid_utf8(tool_call)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool tool_has_valid_utf8(const ai::glaze::ProviderToolDto& tool) {
    return is_valid_utf8(tool.type) &&
           is_valid_utf8(tool.function.name) &&
           is_valid_utf8(tool.function.description) &&
           generic_has_valid_utf8(tool.function.parameters);
}

[[nodiscard]] bool request_has_valid_utf8(const ai::glaze::OpenAIChatRequestDto& request) {
    if (!is_valid_utf8(request.model) ||
        !optional_has_valid_utf8(request.reasoning_effort) ||
        !optional_has_valid_utf8(request.reasoning) ||
        !optional_has_valid_utf8(request.thinking) ||
        !optional_has_valid_utf8(request.max_completion_tokens) ||
        !optional_has_valid_utf8(request.max_tokens)) {
        return false;
    }
    for (const auto& message : request.messages) {
        if (!message_has_valid_utf8(message)) {
            return false;
        }
    }
    if (request.tools) {
        for (const auto& tool : *request.tools) {
            if (!tool_has_valid_utf8(tool)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] util::Expected<std::string> serialize_openai_request(
    const ai::glaze::OpenAIChatRequestDto& request) {
    if (!request_has_valid_utf8(request)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonSerialize,
            "failed to serialize OpenAI request",
            "OpenAI request contains invalid UTF-8"));
    }
    return util::write_json(request);
}

[[nodiscard]] ai::glaze::OpenAIChatRequestDto request_to_openai(
    const ai::StreamChatRequest& request,
    const OpenAIStreamConfig& config,
    std::string_view model) {
    ai::glaze::OpenAIChatRequestDto dto;
    dto.model = model;
    std::string last_emitted_role;
    if (request.context.system_prompt) {
        const auto role = system_role(config.compat);
        dto.messages.push_back(ai::glaze::OpenAIChatMessageDto{
            .role = role,
            .content = *request.context.system_prompt,
        });
        last_emitted_role = role;
    }
    std::vector<const ai::MessageVariant*> model_context_messages;
    model_context_messages.reserve(request.context.messages.size());
    for (const auto& message : request.context.messages) {
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
            bash && bash->exclude_from_context) {
            continue;
        }
        model_context_messages.push_back(&message);
    }

    for (std::size_t index = 0; index < model_context_messages.size(); ++index) {
        const auto& message = *model_context_messages[index];
        if (std::holds_alternative<ai::ToolResultMessage>(message)) {
            std::vector<ai::glaze::OpenAIContentPartDto> image_parts;
            std::size_t group_end = index;
            for (;
                 group_end < model_context_messages.size() &&
                 std::holds_alternative<ai::ToolResultMessage>(*model_context_messages[group_end]);
                 ++group_end) {
                const auto& tool = std::get<ai::ToolResultMessage>(
                    *model_context_messages[group_end]);
                dto.messages.push_back(tool_result_to_openai(tool, config.compat));
                for (const auto& block : tool.content) {
                    if (const auto* image = std::get_if<ai::ImageContent>(&block)) {
                        image_parts.push_back(image_part(*image));
                    }
                }
            }
            index = group_end - 1;

            if (image_parts.empty()) {
                last_emitted_role = "tool";
                continue;
            }

            if (config.compat.requires_assistant_after_tool_result.value_or(false)) {
                append_tool_result_assistant_bridge(dto.messages);
            }

            std::vector<ai::glaze::OpenAIContentPartDto> attachment_parts;
            attachment_parts.reserve(image_parts.size() + 1);
            attachment_parts.push_back(text_part("Attached image(s) from tool result:"));
            attachment_parts.insert(
                attachment_parts.end(),
                std::make_move_iterator(image_parts.begin()),
                std::make_move_iterator(image_parts.end()));
            dto.messages.push_back(ai::glaze::OpenAIChatMessageDto{
                .role = "user",
                .content = std::move(attachment_parts),
            });
            last_emitted_role = "user";
            continue;
        }

        auto converted = message_to_openai(message, config.compat);
        if (converted.role == "user"
            && config.compat.requires_assistant_after_tool_result.value_or(false)
            && last_emitted_role == "tool") {
            append_tool_result_assistant_bridge(dto.messages);
            last_emitted_role = "assistant";
        }
        last_emitted_role = converted.role;
        dto.messages.push_back(std::move(converted));
    }
    if (!request.context.tools.empty()) {
        std::vector<ai::glaze::ProviderToolDto> tools;
        tools.reserve(request.context.tools.size());
        for (const auto& tool : request.context.tools) {
            tools.push_back(ai::glaze::to_provider_tool_dto(tool));
        }
        dto.tools = std::move(tools);
    }
    dto.stream = true;

    // Apply OpenAICompletionsCompat flags
    if (config.compat.supports_store.value_or(false)) {
        dto.store = false;
    }
    if (config.compat.supports_usage_in_streaming.value_or(true)) {
        dto.stream_options = ai::glaze::OpenAIStreamOptionsDto{
            .include_usage = true,
        };
    }
    return dto;
}

[[nodiscard]] ai::Usage normalize_openai_usage(const ai::glaze::OpenAIUsageDto& raw_usage) {
    const auto cache_read = raw_usage.prompt_tokens_details
            && raw_usage.prompt_tokens_details->cached_tokens
        ? *raw_usage.prompt_tokens_details->cached_tokens
        : raw_usage.prompt_cache_hit_tokens.value_or(0);
    const auto cache_write = raw_usage.prompt_tokens_details
        ? raw_usage.prompt_tokens_details->cache_write_tokens.value_or(0)
        : 0;
    const auto input = std::max<std::int64_t>(
        0,
        raw_usage.prompt_tokens - cache_read - cache_write);

    std::optional<std::int64_t> reasoning;
    if (raw_usage.completion_tokens_details) {
        reasoning = raw_usage.completion_tokens_details->reasoning_tokens;
    }

    return ai::Usage{
        .input = input,
        .output = raw_usage.completion_tokens,
        .cache_read = cache_read,
        .cache_write = cache_write,
        .cache_write_1h = std::nullopt,
        .reasoning = reasoning,
        .total_tokens = input + raw_usage.completion_tokens + cache_read + cache_write,
        .cost = ai::UsageCost{
            .input = 0.0,
            .output = 0.0,
            .cache_read = 0.0,
            .cache_write = 0.0,
            .total = 0.0,
        },
    };
}

[[nodiscard]] std::optional<ai::AssistantStopReason> supported_stop_reason_from_provider(
    std::string_view finish_reason) {
    if (finish_reason == "stop" || finish_reason == "end") {
        return ai::AssistantStopReason::Stop;
    }
    if (finish_reason == "length") {
        return ai::AssistantStopReason::Length;
    }
    if (finish_reason == "function_call" || finish_reason == "tool_calls") {
        return ai::AssistantStopReason::ToolUse;
    }
    return std::nullopt;
}

[[nodiscard]] util::ExpectedVoid emit_error(
    ai::AssistantEventSink& sink,
    const util::Error& error,
    ai::AssistantMessage& partial) {
    const auto reason = error.code == util::ErrorCode::Cancelled
        ? ai::AssistantStopReason::Aborted
        : ai::AssistantStopReason::Error;
    partial.stop_reason = reason;
    partial.error_message = bounded_provider_error_detail(
        error.detail.empty() ? error.message : error.detail);
    return emit(sink, ai::AssistantErrorEvent{reason, partial});
}

[[nodiscard]] util::Expected<util::JsonValue> parse_tool_arguments(const std::string& raw_arguments) {
    if (raw_arguments.empty()) {
        return util::read_json<util::JsonValue>("{}");
    }
    return util::read_json<util::JsonValue>(raw_arguments);
}

void finalize_tool_arguments(ai::ToolCallContent& block) {
    auto parsed = parse_tool_arguments(block.raw_arguments);
    if (parsed) {
        block.arguments = std::move(*parsed);
        block.arguments_valid = true;
        block.argument_error = std::nullopt;
        return;
    }

    block.arguments = std::nullopt;
    block.arguments_valid = false;
    block.argument_error = parsed.error().detail.empty()
        ? parsed.error().message
        : parsed.error().detail;
}

[[nodiscard]] util::Expected<ai::glaze::OpenAIStreamChunkDto> read_openai_stream_chunk(std::string_view json) {
    ai::glaze::OpenAIStreamChunkDto chunk;
    auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(chunk, json);
    if (error) {
        return std::unexpected(util::glaze_error(
            error,
            json,
            util::ErrorCode::JsonParse,
            "malformed provider stream JSON"));
    }
    return chunk;
}

} // namespace

StreamingOpenAIChatClient::StreamingOpenAIChatClient(std::shared_ptr<StreamTransport> transport, OpenAIStreamConfig config)
    : transport_(std::move(transport)), config_(std::move(config)) {}

boost::asio::awaitable<util::Expected<ai::AssistantMessage>> StreamingOpenAIChatClient::stream(
    const ai::StreamChatRequest& request,
    ai::AssistantEventSink sink) {
    CCH_TRY(api_key, resolve_api_key());
    if (!transport_) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "missing stream transport",
            "OpenAI streaming client requires a transport"));
    }

    // ADR 0019 precedence rule: a present request Model is authoritative; the
    // configured Model applies when the request carries none.
    const ai::Model& model = request.model ? *request.model : config_.model;
    if (model.id.empty()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "model is required",
            "OpenAI request model must not be empty"));
    }
    if (config_.api.empty()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "API identity is required",
            "OpenAI request API identity must not be empty"));
    }
    if (config_.provider.empty()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "provider identity is required",
            "OpenAI request provider identity must not be empty"));
    }
    if (config_.timeout <= std::chrono::milliseconds::zero()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "request timeout must be positive",
            "OpenAI request timeout must be greater than zero"));
    }

    CCH_TRY(body, serialize_openai_request(request_to_openai(request, config_, model.id)));

    StreamRequest http;
    http.method = "POST";
    http.url = completions_url();
    http.timeout = config_.timeout;
    http.stop_token = request.stop_token;
    http.headers["Authorization"] = "Bearer " + api_key;
    http.headers["Content-Type"] = "application/json";
    http.headers["Accept"] = "text/event-stream";
    if (!config_.organization.empty()) {
        http.headers["OpenAI-Organization"] = config_.organization;
    }
    if (!config_.project.empty()) {
        http.headers["OpenAI-Project"] = config_.project;
    }
    http.body = std::move(body);

    ai::AssistantMessage assistant;
    assistant.api = config_.api;
    assistant.provider = config_.provider;
    assistant.model = model.id;
    assistant.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    CCH_TRY_VOID(emit(sink, ai::AssistantStartEvent{assistant}));

    SseParser parser;
    bool text_started = false;
    bool thinking_started = false;
    bool saw_terminal_choice = false;
    bool saw_assistant_payload = false;
    std::optional<ai::AssistantStopReason> provider_stop_reason;
    std::optional<std::string> unsupported_finish_reason;
    std::optional<std::size_t> text_index;
    std::optional<std::size_t> thinking_index;
    std::map<std::int64_t, ToolCallAccumulator> tool_calls;
    std::optional<util::Error> stream_sink_failure;

    auto emit_stream_event = [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
        auto emitted = emit(sink, event);
        if (!emitted && !stream_sink_failure) {
            stream_sink_failure = emitted.error();
        }
        return emitted;
    };

    auto handle_chunk = [&](std::string_view bytes) -> util::ExpectedVoid {
        auto events = parser.append(bytes);
        if (!events) {
            return std::unexpected(events.error());
        }

        for (const auto& sse_event : *events) {
            if (sse_event.done) {
                continue;
            }
            if (sse_event.data.empty()) {
                continue;
            }

            auto chunk = read_openai_stream_chunk(sse_event.data);
            if (!chunk) {
                return std::unexpected(chunk.error());
            }

            if (!assistant.response_id && chunk->id && !chunk->id->empty()) {
                assistant.response_id = *chunk->id;
            }
            if (!assistant.response_model && chunk->model && !chunk->model->empty() && *chunk->model != model.id) {
                assistant.response_model = *chunk->model;
            }
            if (chunk->usage) {
                assistant.usage = normalize_openai_usage(*chunk->usage);
            } else if (!chunk->choices.empty() && chunk->choices.front().usage) {
                assistant.usage = normalize_openai_usage(*chunk->choices.front().usage);
            }

            for (const auto& choice : chunk->choices) {
                if (choice.finish_reason) {
                    saw_terminal_choice = true;
                    saw_assistant_payload = true;
                    const auto supported = supported_stop_reason_from_provider(*choice.finish_reason);
                    if (!supported) {
                        if (!unsupported_finish_reason) {
                            unsupported_finish_reason = *choice.finish_reason;
                        }
                    } else if (!unsupported_finish_reason) {
                        provider_stop_reason = *supported;
                    }
                }
                if (!choice.delta) {
                    continue;
                }

                if (choice.delta->content && !choice.delta->content->empty()) {
                    saw_assistant_payload = true;
                    if (!text_started) {
                        text_started = true;
                        text_index = assistant.content.size();
                        assistant.content.emplace_back(ai::TextContent{
                            .text = "",
                            .text_signature = std::nullopt,
                        });
                        auto emitted = emit_stream_event(ai::TextStartEvent{*text_index, assistant});
                        if (!emitted) {
                            return std::unexpected(emitted.error());
                        }
                    }
                    auto& text = std::get<ai::TextContent>(assistant.content[*text_index]);
                    text.text += *choice.delta->content;
                    auto emitted = emit_stream_event(ai::TextDeltaEvent{*text_index, *choice.delta->content, assistant});
                    if (!emitted) {
                        return std::unexpected(emitted.error());
                    }
                }

                const std::optional<std::string>* reasoning_delta = nullptr;
                std::string_view reasoning_signature;
                if (choice.delta->reasoning_content && !choice.delta->reasoning_content->empty()) {
                    reasoning_delta = &choice.delta->reasoning_content;
                    reasoning_signature = "reasoning_content";
                } else if (choice.delta->reasoning && !choice.delta->reasoning->empty()) {
                    reasoning_delta = &choice.delta->reasoning;
                    reasoning_signature = config_.provider == "opencode-go"
                        ? "reasoning_content"
                        : "reasoning";
                } else if (choice.delta->reasoning_text && !choice.delta->reasoning_text->empty()) {
                    reasoning_delta = &choice.delta->reasoning_text;
                    reasoning_signature = "reasoning_text";
                }
                if (reasoning_delta != nullptr) {
                    saw_assistant_payload = true;
                    if (!thinking_started) {
                        thinking_started = true;
                        thinking_index = assistant.content.size();
                        assistant.content.emplace_back(ai::ThinkingContent{
                            .thinking = "",
                            .thinking_signature = std::string{reasoning_signature},
                            .redacted = false,
                        });
                        auto emitted = emit_stream_event(ai::ThinkingStartEvent{*thinking_index, assistant});
                        if (!emitted) {
                            return std::unexpected(emitted.error());
                        }
                    }
                    auto& thinking = std::get<ai::ThinkingContent>(assistant.content[*thinking_index]);
                    thinking.thinking += **reasoning_delta;
                    auto emitted = emit_stream_event(ai::ThinkingDeltaEvent{
                        *thinking_index, **reasoning_delta, assistant});
                    if (!emitted) {
                        return std::unexpected(emitted.error());
                    }
                }

                if (choice.delta->tool_calls) {
                    if (!choice.delta->tool_calls->empty()) {
                        saw_assistant_payload = true;
                    }
                    for (const auto& call_delta : *choice.delta->tool_calls) {
                        auto [it, inserted] = tool_calls.try_emplace(
                            call_delta.index,
                            ToolCallAccumulator{"", "", "", assistant.content.size()});
                        auto& call = it->second;
                        if (inserted) {
                            assistant.content.emplace_back(ai::ToolCallContent{});
                            auto emitted = emit_stream_event(ai::ToolCallStartEvent{call.content_index, assistant});
                            if (!emitted) {
                                return std::unexpected(emitted.error());
                            }
                        }
                        std::optional<std::string> argument_delta;
                        if (call_delta.id && call.id.empty()) {
                            call.id = *call_delta.id;
                        }
                        if (call_delta.function) {
                            if (call_delta.function->name && call.name.empty()) {
                                call.name = *call_delta.function->name;
                            }
                            if (call_delta.function->arguments) {
                                argument_delta = *call_delta.function->arguments;
                                call.raw_arguments += *argument_delta;
                            }
                        }

                        auto& block = std::get<ai::ToolCallContent>(assistant.content[call.content_index]);
                        block.id = call.id;
                        block.name = call.name;
                        block.raw_arguments = call.raw_arguments;
                        if (argument_delta) {
                            auto emitted = emit_stream_event(ai::ToolCallDeltaEvent{call.content_index, *argument_delta, assistant});
                            if (!emitted) {
                                return std::unexpected(emitted.error());
                            }
                        }
                    }
                }
            }
        }
        return {};
    };

    auto complete_accepted_failure = [&](const util::Error& error)
        -> util::Expected<ai::AssistantMessage> {
        for (auto& [_, call] : tool_calls) {
            auto& block = std::get<ai::ToolCallContent>(assistant.content[call.content_index]);
            finalize_tool_arguments(block);
        }
        auto emitted = emit_error(sink, error, assistant);
        if (!emitted) {
            return std::unexpected(emitted.error());
        }
        return assistant;
    };

    auto complete_unsupported_finish = [&]() -> util::Expected<ai::AssistantMessage> {
        const auto diagnostic = "Provider finish_reason: " + *unsupported_finish_reason;
        auto error = util::make_error(
            util::ErrorCode::Provider,
            "unsupported provider finish_reason",
            diagnostic);
        return complete_accepted_failure(error);
    };

    auto response = co_await transport_->async_stream(http, handle_chunk);
    if (!response) {
        if (stream_sink_failure) {
            co_return std::unexpected(*stream_sink_failure);
        }
        if (unsupported_finish_reason) {
            co_return complete_unsupported_finish();
        }
        co_return complete_accepted_failure(response.error());
    }

    auto final_event = parser.finish();
    if (!final_event) {
        if (unsupported_finish_reason) {
            co_return complete_unsupported_finish();
        }
        co_return complete_accepted_failure(final_event.error());
    }
    if (*final_event && !(*final_event)->done && !(*final_event)->data.empty()) {
        auto handled = handle_chunk(std::string{"data: " + (*final_event)->data + "\n\n"});
        if (!handled) {
            if (stream_sink_failure) {
                co_return std::unexpected(*stream_sink_failure);
            }
            if (unsupported_finish_reason) {
                co_return complete_unsupported_finish();
            }
            co_return complete_accepted_failure(handled.error());
        }
    }

    if (unsupported_finish_reason) {
        co_return complete_unsupported_finish();
    }

    for (std::size_t content_index = 0; content_index < assistant.content.size(); ++content_index) {
        auto& block = assistant.content[content_index];
        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
            CCH_TRY_VOID(emit(sink, ai::TextEndEvent{content_index, text->text, assistant}));
            continue;
        }
        if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
            CCH_TRY_VOID(emit(sink, ai::ThinkingEndEvent{
                content_index, thinking->thinking, assistant}));
            continue;
        }

        auto& tool_call = std::get<ai::ToolCallContent>(block);
        if (tool_call.id.empty() || tool_call.name.empty()) {
            auto error = util::make_error(
                util::ErrorCode::JsonParse,
                "malformed provider tool call",
                "streamed tool call is missing id or function name");
            co_return complete_accepted_failure(error);
        }
        finalize_tool_arguments(tool_call);
        CCH_TRY_VOID(emit(sink, ai::ToolCallEndEvent{
            content_index, tool_call, assistant}));
    }

    if (!saw_terminal_choice || !provider_stop_reason) {
        auto error = util::make_error(
            util::ErrorCode::Provider,
            "provider stream ended without a finish reason",
            "SSE stream ended without a non-null finish_reason");
        co_return complete_accepted_failure(error);
    }
    if (!saw_assistant_payload) {
        auto error = util::make_error(
            util::ErrorCode::Provider,
            "provider stream contained no assistant payload",
            "SSE stream ended without content, tool calls, or a finish_reason");
        co_return complete_accepted_failure(error);
    }

    assistant.stop_reason = *provider_stop_reason;
    CCH_TRY_VOID(emit(sink, ai::AssistantDoneEvent{assistant.stop_reason, assistant}));

    co_return assistant;
}

util::Expected<std::string> StreamingOpenAIChatClient::resolve_api_key() const {
    if (!config_.api_key.empty()) {
        return config_.api_key;
    }
    if (!config_.api_key_env.empty()) {
        if (const char* value = std::getenv(config_.api_key_env.c_str()); value != nullptr && *value != '\0') {
            return std::string(value);
        }
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Provider,
        "missing API key",
        "set " + config_.api_key_env + " or pass provider configuration"));
}

std::string StreamingOpenAIChatClient::completions_url() const {
    std::string base = config_.base_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.empty()) {
        base = "https://api.openai.com";
    }
    if (base.ends_with("/v1")) {
        return base + "/chat/completions";
    }
    return base + "/v1/chat/completions";
}

} // namespace cch::ai::providers
