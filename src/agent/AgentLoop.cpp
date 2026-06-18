#include "../../include/cch/agent/AgentLoop.hpp"

#include "../../src/util/ExpectedMacros.hpp"

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Usage.hpp"
#include "../../include/cch/util/Json.hpp"

#include <algorithm>
#include <utility>

namespace cch::agent {
namespace {

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

[[nodiscard]] util::Expected<util::JsonValue> arguments_for_call(const ai::ToolCallContent& call) {
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
        return util::read_json<util::JsonValue>("{}");
    }
    return util::read_json<util::JsonValue>(call.raw_arguments);
}

void erase_first(std::vector<std::string>& values, const std::string& value) {
    const auto found = std::find(values.begin(), values.end(), value);
    if (found != values.end()) {
        values.erase(found);
    }
}

void sync_state(AgentState& state, const ai::AiContext& context) {
    state.messages = context.messages;
}

} // namespace

AsyncAgentLoop::AsyncAgentLoop(ai::StreamingChatClient& client, AsyncToolRegistry registry, AsyncAgentOptions options)
    : client_(client), registry_(std::move(registry)), options_(std::move(options)) {}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::run(
    std::string user_prompt,
    AgentEventSink sink) {
    co_return co_await continue_with({}, std::move(user_prompt), std::move(sink));
}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::continue_with(
    std::vector<ai::MessageVariant> history,
    std::string user_prompt,
    AgentEventSink sink) {
    ai::AiContext context;
    context.model = options_.model;
    context.tools = registry_.definitions();
    context.messages = std::move(history);

    AgentState state;
    state.model = options_.model;

    CCH_TRY_VOID(emit(sink, AgentStartEvent{user_prompt}));

    context.messages.push_back(ai::MessageVariant{ai::user_text_message(std::move(user_prompt))});

    for (int turn = 1; turn <= options_.max_turns; ++turn) {
        CCH_TRY_VOID(emit(sink, TurnStartEvent{turn}));
        CCH_TRY_VOID(emit(sink, MessageStartEvent{turn}));

        ai::StreamChatRequest request;
        request.context = context;
        request.model = options_.model;

        auto assistant = co_await client_.stream(
            request,
            [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                if (const auto* start = std::get_if<ai::AssistantStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return {};
                }
                if (const auto* start = std::get_if<ai::TextStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return {};
                }
                if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, MessageUpdateEvent{turn, delta->delta});
                }
                if (const auto* end = std::get_if<ai::TextEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return {};
                }
                if (const auto* start = std::get_if<ai::ThinkingStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return {};
                }
                if (const auto* delta = std::get_if<ai::ThinkingDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, ThinkingUpdateEvent{turn, delta->content_index, delta->delta});
                }
                if (const auto* end = std::get_if<ai::ThinkingEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return {};
                }
                if (const auto* start = std::get_if<ai::ToolCallStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit(sink, ToolCallStreamStartEvent{turn, start->content_index});
                }
                if (const auto* delta = std::get_if<ai::ToolCallDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, ToolCallStreamUpdateEvent{turn, delta->content_index, delta->delta});
                }
                if (const auto* end = std::get_if<ai::ToolCallEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    if (!end->tool_call.id.empty()) {
                        state.pending_tool_call_ids.push_back(end->tool_call.id);
                    }
                    return emit(sink, ToolCallStreamEndEvent{turn, end->content_index, end->tool_call});
                }
                if (const auto* done = std::get_if<ai::AssistantDoneEvent>(&event)) {
                    state.streaming_message = done->message;
                    return {};
                }
                if (const auto* error = std::get_if<ai::AssistantErrorEvent>(&event)) {
                    state.streaming_message = error->error;
                    return {};
                }
                return {};
            });

        if (!assistant) {
            std::string reason = assistant.error().message;
            if (assistant.error().code == util::ErrorCode::Provider && !assistant.error().detail.empty()) {
                reason += " (" + assistant.error().detail + ")";
            }
            CCH_TRY_VOID(emit(sink, AgentEndEvent{false, reason}));
            co_return std::unexpected(assistant.error());
        }

        CCH_TRY_VOID(emit(sink, MessageEndEvent{turn, *assistant}));
        context.messages.push_back(ai::MessageVariant{*assistant});
        state.streaming_message = *assistant;

        auto calls = tool_calls(*assistant);
        state.pending_tool_call_ids.clear();
        for (const auto& call : calls) {
            if (!call.id.empty()) {
                state.pending_tool_call_ids.push_back(call.id);
            }
        }
        if (calls.empty()) {
            state.streaming_message.reset();
            CCH_TRY_VOID(emit(sink, TurnEndEvent{turn, assistant->stop_reason}));
            CCH_TRY_VOID(emit(sink, AgentEndEvent{true, ai::stop_reason_to_string(assistant->stop_reason)}));
            sync_state(state, context);
            co_return AsyncAgentRunResult{std::move(context), assistant->stop_reason, turn, std::move(state)};
        }

        for (const auto& call : calls) {
            state.active_tool_names.push_back(call.name);
            CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{turn, call.id, call.name}));

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
            CCH_TRY_VOID(emit(sink, ToolExecutionEndEvent{turn, call.id, call.name, tool_result.is_error, tool_text}));
            erase_first(state.active_tool_names, call.name);
            erase_first(state.pending_tool_call_ids, call.id);
            context.messages.push_back(ai::MessageVariant{std::move(tool_result)});
        }

        CCH_TRY_VOID(emit(sink, TurnEndEvent{turn, ai::AssistantStopReason::ToolUse}));
    }

    auto error = util::make_error(
        util::ErrorCode::Provider,
        "max turns exceeded",
        "agent reached max_turns before a final assistant response");
    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, error.message}));
    co_return std::unexpected(error);
}

util::ExpectedVoid AsyncAgentLoop::emit(AgentEventSink& sink, const AgentLifecycleEvent& event) const {
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
