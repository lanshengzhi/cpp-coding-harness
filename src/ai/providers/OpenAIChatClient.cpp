#include "../../../include/cch/ai/providers/OpenAIChatClient.hpp"

#include "../../../src/util/ExpectedMacros.hpp"
#include "../glaze/ProviderDtos.hpp"
#include "../../../include/cch/ai/glaze/AiJson.hpp"
#include "../../../include/cch/ai/providers/SseParser.hpp"
#include "../../../include/cch/util/Json.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
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

[[nodiscard]] std::string content_text(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
            text += text_block->text;
        }
    }
    return text;
}

[[nodiscard]] std::vector<ai::glaze::ProviderToolCallDto> tool_calls_from_content(const std::vector<ai::Content>& content) {
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

[[nodiscard]] ai::glaze::OpenAIChatMessageDto message_to_openai(const ai::MessageVariant& message) {
    return std::visit(
        Overloaded{
            [](const ai::SystemMessage& system) {
                return ai::glaze::OpenAIChatMessageDto{"system", system.content, std::nullopt, std::nullopt};
            },
            [](const ai::UserMessage& user) {
                return ai::glaze::OpenAIChatMessageDto{"user", content_text(user.content), std::nullopt, std::nullopt};
            },
            [](const ai::AssistantMessage& assistant) {
                auto calls = tool_calls_from_content(assistant.content);
                std::optional<std::vector<ai::glaze::ProviderToolCallDto>> tool_calls;
                if (!calls.empty()) {
                    tool_calls = std::move(calls);
                }
                return ai::glaze::OpenAIChatMessageDto{
                    "assistant",
                    content_text(assistant.content),
                    std::nullopt,
                    std::move(tool_calls),
                };
            },
            [](const ai::ToolResultMessage& tool) {
                return ai::glaze::OpenAIChatMessageDto{
                    "tool",
                    content_text(tool.content),
                    tool.tool_call_id,
                    std::nullopt,
                };
            },
        },
        message);
}

[[nodiscard]] ai::glaze::OpenAIChatRequestDto request_to_openai(
    const ai::StreamChatRequest& request,
    const OpenAIStreamConfig& config) {
    ai::glaze::OpenAIChatRequestDto dto;
    dto.model = !request.model.empty() ? request.model : (!request.context.model.empty() ? request.context.model : config.model);
    if (request.context.system_prompt) {
        dto.messages.push_back(ai::glaze::OpenAIChatMessageDto{"system", *request.context.system_prompt, std::nullopt, std::nullopt});
    }
    for (const auto& message : request.context.messages) {
        dto.messages.push_back(message_to_openai(message));
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
    assistant.api = "openai-chat-completions";
    assistant.provider = "openai";
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

            auto chunk = util::read_json<ai::glaze::OpenAIStreamChunkDto>(sse_event.data);
            if (!chunk) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::JsonParse,
                    "malformed provider stream JSON",
                    chunk.error().detail,
                    sse_event.data));
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
                        if (call_delta.id) {
                            call.id = *call_delta.id;
                        }
                        if (call_delta.function) {
                            if (call_delta.function->name) {
                                call.name += *call_delta.function->name;
                            }
                            if (call_delta.function->arguments) {
                                call.raw_arguments += *call_delta.function->arguments;
                                auto emitted = emit(sink, ai::ToolCallDeltaEvent{call.content_index, *call_delta.function->arguments, assistant});
                                if (!emitted) {
                                    return std::unexpected(emitted.error());
                                }
                            }
                        }

                        auto& block = std::get<ai::ToolCallContent>(assistant.content[call.content_index]);
                        block.id = call.id;
                        block.name = call.name;
                        block.raw_arguments = call.raw_arguments;
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
    if (base.size() >= 3 && base.substr(base.size() - 3) == "/v1") {
        return base + "/chat/completions";
    }
    return base + "/v1/chat/completions";
}

} // namespace cch::ai::providers
