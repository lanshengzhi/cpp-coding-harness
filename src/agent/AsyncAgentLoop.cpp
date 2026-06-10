#include <cch/agent/AgentLoop.hpp>

#include <cch/ai/glaze/AiJson.hpp>

#include <utility>

namespace cch::agent {
namespace {

[[nodiscard]] std::string text_from_content(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
            text += text_block->text;
        }
    }
    return text;
}

[[nodiscard]] ai::ToolResultMessage error_tool_result(
    const ai::ToolCallContent& call,
    std::string message) {
    ai::ToolResultMessage result;
    result.tool_call_id = call.id;
    result.tool_name = call.name;
    result.content.emplace_back(ai::TextContent{std::move(message), std::nullopt});
    result.is_error = true;
    return result;
}

[[nodiscard]] util::Expected<glz::generic> arguments_for_call(const ai::ToolCallContent& call) {
    if (!call.arguments_valid) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "invalid tool arguments",
            call.argument_error.value_or("tool arguments were malformed")));
    }
    if (call.arguments) {
        return *call.arguments;
    }
    if (call.raw_arguments.empty()) {
        return util::read_json<glz::generic>("{}");
    }
    return util::read_json<glz::generic>(call.raw_arguments);
}

} // namespace

AsyncAgentLoop::AsyncAgentLoop(ai::StreamingChatClient& client, AsyncToolRegistry registry, AsyncAgentOptions options)
    : client_(client), registry_(std::move(registry)), options_(std::move(options)) {}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::run(
    std::string user_prompt,
    AgentEventSink sink) {
    ai::AiContext context;
    context.model = options_.model;
    context.tools = registry_.definitions();

    if (auto emitted = emit(sink, AgentStartEvent{user_prompt}); !emitted) {
        co_return std::unexpected(emitted.error());
    }

    context.messages.push_back(ai::MessageVariant{ai::user_text_message(std::move(user_prompt))});

    for (int turn = 1; turn <= options_.max_turns; ++turn) {
        if (auto emitted = emit(sink, TurnStartEvent{turn}); !emitted) {
            co_return std::unexpected(emitted.error());
        }
        if (auto emitted = emit(sink, MessageStartEvent{turn}); !emitted) {
            co_return std::unexpected(emitted.error());
        }

        ai::StreamChatRequest request;
        request.context = context;
        request.model = options_.model;

        auto assistant = co_await client_.stream(
            request,
            [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&event)) {
                    return emit(sink, MessageUpdateEvent{turn, delta->delta});
                }
                return {};
            });

        if (!assistant) {
            emit(sink, AgentEndEvent{false, assistant.error().message});
            co_return std::unexpected(assistant.error());
        }

        if (auto emitted = emit(sink, MessageEndEvent{turn, *assistant}); !emitted) {
            co_return std::unexpected(emitted.error());
        }
        context.messages.push_back(ai::MessageVariant{*assistant});

        auto calls = tool_calls(*assistant);
        if (calls.empty()) {
            if (auto emitted = emit(sink, TurnEndEvent{turn, assistant->stop_reason}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
            if (auto emitted = emit(sink, AgentEndEvent{true, ai::stop_reason_to_json(assistant->stop_reason)}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
            co_return AsyncAgentRunResult{std::move(context), assistant->stop_reason, turn};
        }

        for (const auto& call : calls) {
            if (auto emitted = emit(sink, ToolExecutionStartEvent{turn, call.id, call.name}); !emitted) {
                co_return std::unexpected(emitted.error());
            }

            ai::ToolResultMessage tool_result;
            auto* tool = registry_.find(call.name);
            if (tool == nullptr) {
                tool_result = error_tool_result(call, "unknown tool: " + call.name);
            } else {
                auto arguments = arguments_for_call(call);
                if (!arguments) {
                    tool_result = error_tool_result(call, arguments.error().detail);
                } else {
                    ToolInvocation invocation{call.id, call.name, std::move(*arguments), call.raw_arguments};
                    auto executed = co_await tool->execute(invocation);
                    if (!executed) {
                        tool_result = error_tool_result(call, executed.error().detail.empty() ? executed.error().message : executed.error().detail);
                    } else {
                        tool_result.tool_call_id = call.id;
                        tool_result.tool_name = call.name;
                        tool_result.content.emplace_back(ai::TextContent{executed->content, std::nullopt});
                        tool_result.details = executed->details;
                        tool_result.is_error = executed->is_error;
                    }
                }
            }

            const auto tool_text = text_from_content(tool_result.content);
            if (auto emitted = emit(sink, ToolExecutionEndEvent{turn, call.id, call.name, tool_result.is_error, tool_text}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
            context.messages.push_back(ai::MessageVariant{std::move(tool_result)});
        }

        if (auto emitted = emit(sink, TurnEndEvent{turn, ai::AssistantStopReason::ToolUse}); !emitted) {
            co_return std::unexpected(emitted.error());
        }
    }

    auto error = util::make_error(
        util::ErrorCode::Provider,
        "max turns exceeded",
        "agent reached max_turns before a final assistant response");
    if (auto emitted = emit(sink, AgentEndEvent{false, error.message}); !emitted) {
        co_return std::unexpected(emitted.error());
    }
    co_return std::unexpected(error);
}

util::ExpectedVoid AsyncAgentLoop::emit(const AgentEventSink& sink, const AgentLifecycleEvent& event) const {
    if (!sink) {
        return {};
    }
    return sink(event);
}

std::vector<ai::ToolCallContent> AsyncAgentLoop::tool_calls(const ai::AssistantMessage& message) const {
    std::vector<ai::ToolCallContent> calls;
    for (const auto& block : message.content) {
        if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
            calls.push_back(*call);
        }
    }
    return calls;
}

} // namespace cch::agent
