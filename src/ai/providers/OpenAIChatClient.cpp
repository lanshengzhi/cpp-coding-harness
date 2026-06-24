#include "../../../include/cch/ai/providers/OpenAIChatClient.hpp"

#include "../../../src/util/ExpectedMacros.hpp"
#include "../glaze/ProviderDtos.hpp"
#include "ai/glaze/AiJson.hpp"
#include "../../../include/cch/ai/providers/OpenAICompletionsCompat.hpp"
#include "ai/providers/SseParser.hpp"
#include "util/Json.hpp"

#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace cch::ai::providers {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
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

[[nodiscard]] std::string content_text(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        std::visit(
            Overloaded{
                [&text](const ai::TextContent& text_block) {
                    append_plain_text_part(text, text_block.text, "\n");
                },
                [&text](const ai::ThinkingContent&) {
                    append_plain_text_part(text, "[thinking content omitted]", "\n");
                },
                [&text](const ai::ImageContent& image) {
                    append_plain_text_part(text, "[image content omitted: " + image.mime_type + "]", "\n");
                },
            },
            block);
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
                call->id,
                "function",
                ai::glaze::ProviderToolCallFunctionDto{call->name, call->raw_arguments},
            });
        }
    }
    return calls;
}

[[nodiscard]] ai::glaze::OpenAIChatMessageDto message_to_openai(
    const ai::MessageVariant& message,
    const OpenAICompletionsCompat& compat = {}) {
    return std::visit(
        Overloaded{
            [&compat](const ai::SystemMessage& system) {
                return ai::glaze::OpenAIChatMessageDto{system_role(compat), system.content, std::nullopt, std::nullopt, std::nullopt};
            },
            [](const ai::UserMessage& user) {
                return ai::glaze::OpenAIChatMessageDto{"user", content_text(user.content), std::nullopt, std::nullopt, std::nullopt};
            },
            [&compat](const ai::AssistantMessage& assistant) {
                auto calls = tool_calls_from_assistant_content(assistant.content);
                std::optional<std::vector<ai::glaze::ProviderToolCallDto>> tool_calls;
                if (!calls.empty()) {
                    tool_calls = std::move(calls);
                }
                return ai::glaze::OpenAIChatMessageDto{
                    "assistant",
                    assistant_content_text(assistant.content, compat),
                    std::nullopt,
                    std::nullopt,
                    std::move(tool_calls),
                };
            },
            [&compat](const ai::ToolResultMessage& tool) {
                auto dto = ai::glaze::OpenAIChatMessageDto{
                    "tool",
                    content_text(tool.content),
                    std::nullopt,
                    tool.tool_call_id,
                    std::nullopt,
                };
                if (compat.requires_tool_result_name.value_or(false)) {
                    dto.name = tool.tool_name;
                }
                return dto;
            },
            [](const ai::BashExecutionMessage& bash) {
                auto msg = bash_execution_to_user_message(bash);
                return ai::glaze::OpenAIChatMessageDto{"user", content_text(msg.content), std::nullopt, std::nullopt, std::nullopt};
            },
            [](const ai::CustomMessage& custom) {
                auto msg = custom_message_to_user_message(custom);
                return ai::glaze::OpenAIChatMessageDto{"user", content_text(msg.content), std::nullopt, std::nullopt, std::nullopt};
            },
            [](const ai::BranchSummaryMessage& branch) {
                auto msg = branch_summary_to_user_message(branch);
                return ai::glaze::OpenAIChatMessageDto{"user", content_text(msg.content), std::nullopt, std::nullopt, std::nullopt};
            },
            [](const ai::CompactionSummaryMessage& compaction) {
                auto msg = compaction_summary_to_user_message(compaction);
                return ai::glaze::OpenAIChatMessageDto{"user", content_text(msg.content), std::nullopt, std::nullopt, std::nullopt};
            },
        },
        message);
}

[[nodiscard]] ai::glaze::OpenAIChatRequestDto request_to_openai(
    const ai::StreamChatRequest& request,
    const OpenAIStreamConfig& config) {
    ai::glaze::OpenAIChatRequestDto dto;
    dto.model = !request.model.empty() ? request.model : (!request.context.model.empty() ? request.context.model : config.model);
    std::string last_emitted_role;
    if (request.context.system_prompt) {
        const auto role = system_role(config.compat);
        dto.messages.push_back(ai::glaze::OpenAIChatMessageDto{role, *request.context.system_prompt, std::nullopt, std::nullopt, std::nullopt});
        last_emitted_role = role;
    }
    for (const auto& message : request.context.messages) {
        if (const auto* bash = std::get_if<ai::BashExecutionMessage>(&message); bash && bash->exclude_from_context) {
            continue;
        }
        auto converted = message_to_openai(message, config.compat);
        if (converted.role == "user"
            && config.compat.requires_assistant_after_tool_result.value_or(false)
            && last_emitted_role == "tool") {
            dto.messages.push_back(ai::glaze::OpenAIChatMessageDto{
                "assistant",
                "I have processed the tool results.",
                std::nullopt,
                std::nullopt,
                std::nullopt,
            });
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
        dto.stream_options = ai::glaze::OpenAIStreamOptionsDto{true};
    }
    return dto;
}

[[nodiscard]] ai::AssistantStopReason stop_reason_from_provider(const std::optional<std::string>& finish_reason) {
    if (!finish_reason) {
        return ai::AssistantStopReason::Unknown;
    }
    return ai::glaze::stop_reason_from_json(*finish_reason);
}

[[nodiscard]] util::ExpectedVoid emit(ai::AssistantEventSink& sink, const ai::AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    return sink(event);
}

[[nodiscard]] util::ExpectedVoid emit_error(
    ai::AssistantEventSink& sink,
    const util::Error& error,
    ai::AssistantMessage partial) {
    partial.stop_reason = ai::AssistantStopReason::Error;
    partial.error_message = error.detail.empty() ? error.message : error.detail;
    return emit(sink, ai::AssistantErrorEvent{ai::AssistantStopReason::Error, std::move(partial)});
}

[[nodiscard]] util::Expected<util::JsonValue> parse_tool_arguments(const std::string& raw_arguments) {
    if (raw_arguments.empty()) {
        return util::read_json<util::JsonValue>("{}");
    }
    return util::read_json<util::JsonValue>(raw_arguments);
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
            util::ErrorCode::Network,
            "missing stream transport",
            "OpenAI streaming client requires a transport"));
    }

    CCH_TRY(body, util::write_json(request_to_openai(request, config_)));

    StreamRequest http;
    http.method = "POST";
    http.url = completions_url();
    http.timeout = config_.timeout;
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
    assistant.model = request.model.empty() ? (request.context.model.empty() ? config_.model : request.context.model) : request.model;
    assistant.stop_reason = ai::AssistantStopReason::Unknown;

    CCH_TRY_VOID(emit(sink, ai::AssistantStartEvent{assistant}));

    SseParser parser;
    bool text_started = false;
    bool saw_done = false;
    bool saw_terminal_choice = false;
    bool saw_assistant_payload = false;
    std::optional<std::size_t> text_index;
    std::map<std::int64_t, ToolCallAccumulator> tool_calls;

    auto handle_chunk = [&](std::string_view bytes) -> util::ExpectedVoid {
        auto events = parser.append(bytes);
        if (!events) {
            return std::unexpected(events.error());
        }

        for (const auto& sse_event : *events) {
            if (sse_event.done) {
                saw_done = true;
                continue;
            }
            if (sse_event.data.empty()) {
                continue;
            }

            auto chunk = read_openai_stream_chunk(sse_event.data);
            if (!chunk) {
                return std::unexpected(chunk.error());
            }

            if (chunk->id) {
                assistant.response_id = chunk->id;
            }
            if (chunk->model) {
                assistant.response_model = chunk->model;
            }
            if (chunk->usage) {
                assistant.usage = ai::Usage{
                    chunk->usage->prompt_tokens,
                    chunk->usage->completion_tokens,
                    0,
                    0,
                    chunk->usage->total_tokens,
                    {},
                };
            }

            for (const auto& choice : chunk->choices) {
                if (choice.finish_reason) {
                    saw_terminal_choice = true;
                    saw_assistant_payload = true;
                    assistant.stop_reason = stop_reason_from_provider(choice.finish_reason);
                }
                if (!choice.delta) {
                    continue;
                }

                if (choice.delta->content && !choice.delta->content->empty()) {
                    saw_assistant_payload = true;
                    if (!text_started) {
                        text_started = true;
                        text_index = assistant.content.size();
                        assistant.content.emplace_back(ai::TextContent{"", std::nullopt});
                        auto emitted = emit(sink, ai::TextStartEvent{*text_index, assistant});
                        if (!emitted) {
                            return std::unexpected(emitted.error());
                        }
                    }
                    auto& text = std::get<ai::TextContent>(assistant.content[*text_index]);
                    text.text += *choice.delta->content;
                    auto emitted = emit(sink, ai::TextDeltaEvent{*text_index, *choice.delta->content, assistant});
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
                            auto emitted = emit(sink, ai::ToolCallStartEvent{call.content_index, assistant});
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
                            auto emitted = emit(sink, ai::ToolCallDeltaEvent{call.content_index, *argument_delta, assistant});
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

    auto response = co_await transport_->async_stream(http, handle_chunk);
    if (!response) {
        CCH_TRY_VOID(emit_error(sink, response.error(), assistant));
        co_return std::unexpected(response.error());
    }

    auto final_event = parser.finish();
    if (!final_event) {
        CCH_TRY_VOID(emit_error(sink, final_event.error(), assistant));
        co_return std::unexpected(final_event.error());
    }
    if (*final_event) {
        if ((*final_event)->done) {
            saw_done = true;
        } else if (!(*final_event)->data.empty()) {
            auto handled = handle_chunk(std::string{"data: " + (*final_event)->data + "\n\n"});
            if (!handled) {
                CCH_TRY_VOID(emit_error(sink, handled.error(), assistant));
                co_return std::unexpected(handled.error());
            }
        }
    }

    if (!saw_done && !saw_terminal_choice) {
        auto error = util::make_error(
            util::ErrorCode::Provider,
            "provider stream ended before terminal event",
            "SSE stream ended without [DONE] or a finish_reason");
        CCH_TRY_VOID(emit_error(sink, error, assistant));
        co_return std::unexpected(error);
    }
    if (!saw_assistant_payload) {
        auto error = util::make_error(
            util::ErrorCode::Provider,
            "provider stream contained no assistant payload",
            "SSE stream ended without content, tool calls, or a finish_reason");
        CCH_TRY_VOID(emit_error(sink, error, assistant));
        co_return std::unexpected(error);
    }

    if (text_started && text_index) {
        const auto& text = std::get<ai::TextContent>(assistant.content[*text_index]);
        CCH_TRY_VOID(emit(sink, ai::TextEndEvent{*text_index, text.text, assistant}));
    }

    for (auto& [_, call] : tool_calls) {
        auto& block = std::get<ai::ToolCallContent>(assistant.content[call.content_index]);
        if (block.id.empty() || block.name.empty()) {
            auto error = util::make_error(
                util::ErrorCode::JsonParse,
                "malformed provider tool call",
                "streamed tool call is missing id or function name");
            CCH_TRY_VOID(emit_error(sink, error, assistant));
            co_return std::unexpected(error);
        }
        static_cast<void>(parse_tool_arguments(block.raw_arguments)
            .transform([&](util::JsonValue&& parsed) {
                block.arguments = std::move(parsed);
                block.arguments_valid = true;
                block.argument_error = std::nullopt;
            })
            .or_else([&](const util::Error& err) -> util::ExpectedVoid {
                block.arguments = std::nullopt;
                block.arguments_valid = false;
                block.argument_error = err.detail.empty() ? err.message : err.detail;
                return {};
            }));
        CCH_TRY_VOID(emit(sink, ai::ToolCallEndEvent{call.content_index, block, assistant}));
    }

    if (assistant.stop_reason == ai::AssistantStopReason::Unknown) {
        assistant.stop_reason = tool_calls.empty() ? ai::AssistantStopReason::Stop : ai::AssistantStopReason::ToolUse;
    }

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
    if (base.size() >= 3 && base.substr(base.size() - 3) == "/v1") {
        return base + "/chat/completions";
    }
    return base + "/v1/chat/completions";
}

} // namespace cch::ai::providers
