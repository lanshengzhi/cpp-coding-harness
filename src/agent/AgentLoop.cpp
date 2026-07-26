#include "AgentLoop.hpp"

#include "ExecutionShared.hpp"
#include "ToolCallExecutor.hpp"

#include "../../src/util/ExpectedMacros.hpp"

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Usage.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

void sync_state(AgentState& state, const ai::AiContext& context) {
    state.messages = context.messages;
}

[[nodiscard]] util::ExpectedVoid append_message_with_lifecycle(
    AgentState& state,
    ai::AiContext& context,
    AgentEventSink& sink,
    ai::MessageVariant message) {
    if (auto r = emit_agent_event(sink, MessageStartEvent{message}); !r) {
        return r;
    }
    context.messages.push_back(std::move(message));
    sync_state(state, context);
    return emit_agent_event(sink, MessageEndEvent{context.messages.back()});
}

[[nodiscard]] std::size_t approximate_content_size(const ai::Content& block) {
    return std::visit(
        [](const auto& c) -> std::size_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(c)>, ai::TextContent>) {
                return c.text.size();
            } else if constexpr (std::is_same_v<std::decay_t<decltype(c)>, ai::ImageContent>) {
                return c.data.size() + c.mime_type.size();
            } else if constexpr (std::is_same_v<std::decay_t<decltype(c)>, ai::ThinkingContent>) {
                return c.thinking.size();
            }
            return 0;
        },
        block);
}

[[nodiscard]] std::size_t approximate_message_size(const ai::MessageVariant& message) {
    return std::visit(
        [](const auto& m) -> std::size_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::UserMessage>) {
                std::size_t size = 0;
                for (const auto& block : m.content) {
                    size += approximate_content_size(block);
                }
                return size;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::AssistantMessage>) {
                std::size_t size = 0;
                for (const auto& block : m.content) {
                    if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                        size += text->text.size();
                    } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                        size += thinking->thinking.size();
                    } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                        size += call->raw_arguments.size();
                    }
                }
                return size;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::ToolResultMessage>) {
                return ai::text_from_content(m.content).size();
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::SystemMessage>) {
                return m.content.size();
            }
            return 0;
        },
        message);
}

// A rejected steering/follow-up admission never aborts the active run and
// never modifies already-queued messages (ADR 0022): the batch is not
// admitted and the rejection is reported as a bounded diagnostic instead.
void record_queue_rejection(AgentState& state, util::Error error) {
    constexpr std::size_t kMaxQueueRejectionDiagnostics = 16;
    if (state.diagnostics.size() == kMaxQueueRejectionDiagnostics) {
        state.diagnostics.erase(state.diagnostics.begin());
    }
    state.diagnostics.push_back(std::move(error));
}

[[nodiscard]] util::ExpectedVoid validate_queued_messages(
    const std::vector<ai::MessageVariant>& messages,
    std::size_t max_queued_messages,
    std::size_t max_queued_bytes) {
    if (messages.size() > max_queued_messages) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "too many queued messages",
            "steering/follow-up message count exceeds " + std::to_string(max_queued_messages)));
    }

    std::size_t total = 0;
    for (const auto& message : messages) {
        total += approximate_message_size(message);
    }
    if (total > max_queued_bytes) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "queued messages too large",
            "steering/follow-up message byte size exceeds " + std::to_string(max_queued_bytes)));
    }
    return {};
}

[[nodiscard]] bool is_valid_thinking_level(std::string_view level) {
    static const std::vector<std::string> allowed{
        "off", "minimal", "low", "medium", "high", "xhigh"};
    return std::find(allowed.begin(), allowed.end(), level) != allowed.end();
}

[[nodiscard]] util::ExpectedVoid apply_turn_update(
    AsyncAgentOptions& options,
    ai::AiContext& context,
    AgentState& state,
    const AgentLoopTurnUpdate& update) {
    if (update.model && update.model->id.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid model",
            "model update cannot be empty"));
    }
    if (update.thinking_level && !is_valid_thinking_level(*update.thinking_level)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid thinking level",
            *update.thinking_level));
    }

    if (update.context) {
        context.system_prompt = update.context->system_prompt;
        context.messages = update.context->messages;
        sync_state(state, context);
    }

    if (update.model) {
        options.model = *update.model;
        state.model = *update.model;
    }

    if (update.thinking_level) {
        options.thinking_level = *update.thinking_level;
        state.thinking_level = *update.thinking_level;
    }

    return {};
}

} // namespace

AsyncAgentLoop::AsyncAgentLoop(ai::StreamingChatClient& client, AsyncToolRegistry registry, AsyncAgentOptions options)
    : client_(client), registry_(std::move(registry)), options_(std::move(options)) {}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::run(
    std::string user_prompt,
    AgentEventSink sink,
    std::stop_token stop_token) {
    co_return co_await continue_with(
        {}, std::move(user_prompt), std::move(sink), stop_token);
}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::continue_with(
    std::vector<ai::MessageVariant> history,
    std::string user_prompt,
    AgentEventSink sink,
    std::stop_token stop_token) {
    ai::AiContext context;
    context.tools = registry_.definitions();
    context.messages = std::move(history);
    std::vector<ai::MessageVariant> new_messages;
    const auto emit_agent_end = [&]() -> util::ExpectedVoid {
        return emit_agent_event(sink, AgentEndEvent{new_messages});
    };

    AgentState state;
    state.model = options_.model;
    state.thinking_level = options_.thinking_level;

    CCH_TRY_VOID(emit_agent_event(sink, AgentStartEvent{}));

    ai::MessageVariant user_message = ai::user_text_message(std::move(user_prompt));

    std::vector<ai::MessageVariant> pending_messages;

    for (int turn = 1; !options_.max_turns || turn <= *options_.max_turns; ++turn) {
        CCH_TRY_VOID(emit_agent_event(sink, TurnStartEvent{}));

        if (turn == 1) {
            CCH_TRY_VOID(append_message_with_lifecycle(state, context, sink, std::move(user_message)));
            new_messages.push_back(context.messages.back());
        }

        if (turn == 1 && options_.get_steering_messages) {
            auto steering = invoke_sync_agent_hook(
                "getSteeringMessages", *options_.get_steering_messages);
            if (!steering) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(steering.error());
            }
            if (auto validated = validate_queued_messages(
                    *steering, options_.max_queued_messages, options_.max_queued_bytes);
                !validated) {
                record_queue_rejection(state, validated.error());
            } else {
                pending_messages = std::move(*steering);
            }
        }

        if (!pending_messages.empty()) {
            for (auto& message : pending_messages) {
                CCH_TRY_VOID(append_message_with_lifecycle(state, context, sink, std::move(message)));
                new_messages.push_back(context.messages.back());
            }
            pending_messages.clear();
        }

        ai::StreamChatRequest request;
        request.model = options_.model;

        {
            ai::AiContext request_context = context;
            if (options_.transform_context) {
                auto transformed = co_await invoke_agent_hook(
                    "transformContext",
                    *options_.transform_context,
                    std::move(request_context.messages),
                    stop_token);
                if (!transformed) {
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(transformed.error());
                }
                request_context.messages = std::move(*transformed);
            }
            if (options_.convert_to_llm) {
                auto converted = co_await invoke_agent_hook(
                    "convertToLlm",
                    *options_.convert_to_llm,
                    std::move(request_context.messages));
                if (!converted) {
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(converted.error());
                }
                if (converted->empty()) {
                    auto error = util::make_error(
                        util::ErrorCode::Validation,
                        "convertToLlm returned no messages",
                        "LLM request would be empty");
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(error);
                }
                request_context.messages = std::move(*converted);
            }
            request.context = std::move(request_context);
        }

        // Tracks whether the provider emitted an assistant start event for
        // this response, so a terminal-before-start sequence can be recovered
        // with one synthesized start (matching pi's addedPartial rule).
        bool assistant_start_emitted = false;
        auto assistant = co_await client_.stream(
            request,
            [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                if (const auto* start = std::get_if<ai::AssistantStartEvent>(&event)) {
                    if (assistant_start_emitted) {
                        return {};
                    }
                    assistant_start_emitted = true;
                    state.streaming_message = start->partial;
                    return emit_agent_event(sink, MessageStartEvent{ai::MessageVariant{start->partial}});
                }
                if (const auto* start = std::get_if<ai::TextStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{start->partial}, event});
                }
                if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{delta->partial}, event});
                }
                if (const auto* end = std::get_if<ai::TextEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{end->partial}, event});
                }
                if (const auto* start = std::get_if<ai::ThinkingStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{start->partial}, event});
                }
                if (const auto* delta = std::get_if<ai::ThinkingDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{delta->partial}, event});
                }
                if (const auto* end = std::get_if<ai::ThinkingEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{end->partial}, event});
                }
                if (const auto* start = std::get_if<ai::ToolCallStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{start->partial}, event});
                }
                if (const auto* delta = std::get_if<ai::ToolCallDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{delta->partial}, event});
                }
                if (const auto* end = std::get_if<ai::ToolCallEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    if (!end->tool_call.id.empty()) {
                        state.pending_tool_call_ids.push_back(end->tool_call.id);
                    }
                    return emit_agent_event(sink, MessageUpdateEvent{ai::MessageVariant{end->partial}, event});
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
            CCH_TRY_VOID(emit_agent_end());
            co_return std::unexpected(assistant.error());
        }

        context.messages.push_back(ai::MessageVariant{*assistant});
        new_messages.push_back(context.messages.back());
        state.streaming_message = *assistant;
        sync_state(state, context);
        if (!assistant_start_emitted) {
            // A conforming host provider may reach its terminal event before
            // any assistant start event. Synthesize the missing start from the
            // authoritative final message so consumers observe exactly one
            // complete assistant lifecycle.
            CCH_TRY_VOID(emit_agent_event(sink, MessageStartEvent{context.messages.back()}));
        }
        CCH_TRY_VOID(emit_agent_event(sink, MessageEndEvent{context.messages.back()}));

        if (assistant->stop_reason == ai::AssistantStopReason::Error ||
            assistant->stop_reason == ai::AssistantStopReason::Aborted) {
            state.pending_tool_call_ids.clear();
            CCH_TRY_VOID(emit_agent_event(sink, TurnEndEvent{ai::MessageVariant{*assistant}, {}}));
            state.streaming_message.reset();
            CCH_TRY_VOID(emit_agent_end());
            co_return AsyncAgentRunResult{
                std::move(context), assistant->stop_reason, turn, std::move(state)};
        }

        auto calls = tool_calls_from(*assistant);
        state.pending_tool_call_ids.clear();
        for (const auto& call : calls) {
            if (!call.id.empty()) {
                state.pending_tool_call_ids.push_back(call.id);
            }
        }

        std::vector<ai::ToolResultMessage> tool_results;
        bool terminate_batch = false;
        if (!calls.empty() && assistant->stop_reason == ai::AssistantStopReason::Length) {
            tool_results.reserve(calls.size());
            for (const auto& call : calls) {
                CCH_TRY_VOID(emit_agent_event(sink, ToolExecutionStartEvent{call.id, call.name, call.arguments.value_or(util::JsonValue{})}));

                ai::ToolResultMessage result;
                result.tool_call_id = call.id;
                result.tool_name = call.name;
                result.is_error = true;
                result.content.emplace_back(ai::text_content(
                    "Tool call \"" + call.name +
                    "\" was not executed because the response hit the output token limit and its arguments may be "
                    "truncated. Re-issue the tool call with complete arguments."));

                AsyncToolExecutionResult execution_result;
                execution_result.content = result.content;
                execution_result.is_error = true;
                CCH_TRY_VOID(emit_agent_event(sink, ToolExecutionEndEvent{
                    call.id, call.name, std::move(execution_result), true}));
                CCH_TRY_VOID(emit_tool_result_message(sink, result));
                tool_results.push_back(std::move(result));
            }
        } else if (!calls.empty()) {
            agent::ToolCallExecutorOptions executor_options;
            executor_options.before_tool_call =
                options_.before_tool_call ? &*options_.before_tool_call : nullptr;
            executor_options.after_tool_call =
                options_.after_tool_call ? &*options_.after_tool_call : nullptr;
            executor_options.stop_token = stop_token;
            executor_options.execution = options_.tool_execution;
            agent::ToolCallExecutor executor{registry_, std::move(executor_options)};

            auto execution = co_await executor.execute(
                agent::ToolCallBatchRequest{*assistant, context},
                sink);
            if (!execution) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(execution.error());
            }

            tool_results = std::move(execution->results);
            terminate_batch = execution->terminate_batch;
        }

        if (!tool_results.empty()) {
            for (const auto& result : tool_results) {
                context.messages.push_back(ai::MessageVariant{result});
                new_messages.push_back(context.messages.back());
            }
            state.active_tool_names.clear();
            state.pending_tool_call_ids.clear();
            sync_state(state, context);
        }

        CCH_TRY_VOID(emit_agent_event(sink, TurnEndEvent{ai::MessageVariant{*assistant}, tool_results}));

        const bool has_more_tool_calls = !calls.empty() && !terminate_batch;

        PrepareNextTurnContext next_turn_context;
        next_turn_context.assistant_message = *assistant;
        next_turn_context.tool_results = tool_results;
        next_turn_context.context = context;
        next_turn_context.new_messages = new_messages;

        if (options_.prepare_next_turn) {
            auto update = co_await invoke_agent_hook(
                "prepareNextTurn", *options_.prepare_next_turn, next_turn_context);
            if (!update) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(update.error());
            }
            if (*update) {
                if ((**update).model && !options_.validate_turn_update) {
                    auto error = util::make_error(
                        util::ErrorCode::Validation,
                        "model update requires validation",
                        (**update).model->id);
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(error);
                }
                if (options_.validate_turn_update) {
                    auto validated = co_await invoke_agent_hook(
                        "validateTurnUpdate", *options_.validate_turn_update, **update);
                    if (!validated) {
                        CCH_TRY_VOID(emit_agent_end());
                        co_return std::unexpected(validated.error());
                    }
                }
                if (auto applied = apply_turn_update(
                        options_, context, state, **update);
                    !applied) {
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(applied.error());
                }
            }
        }

        next_turn_context.context = context;
        if (options_.should_stop_after_turn) {
            auto should_stop = co_await invoke_agent_hook(
                "shouldStopAfterTurn", *options_.should_stop_after_turn, next_turn_context);
            if (!should_stop) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(should_stop.error());
            }
            if (*should_stop) {
                state.streaming_message.reset();
                CCH_TRY_VOID(emit_agent_end());
                sync_state(state, context);
                co_return AsyncAgentRunResult{
                    std::move(context), assistant->stop_reason, turn, std::move(state)};
            }
        }

        if (options_.get_steering_messages) {
            auto steering = invoke_sync_agent_hook(
                "getSteeringMessages", *options_.get_steering_messages);
            if (!steering) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(steering.error());
            }
            if (auto validated = validate_queued_messages(
                    *steering, options_.max_queued_messages, options_.max_queued_bytes);
                !validated) {
                record_queue_rejection(state, validated.error());
            } else {
                pending_messages = std::move(*steering);
            }
        }

        if (!has_more_tool_calls && pending_messages.empty() && options_.get_follow_up_messages) {
            auto follow_up = invoke_sync_agent_hook(
                "getFollowUpMessages", *options_.get_follow_up_messages);
            if (!follow_up) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(follow_up.error());
            }
            if (auto validated = validate_queued_messages(
                    *follow_up, options_.max_queued_messages, options_.max_queued_bytes);
                !validated) {
                record_queue_rejection(state, validated.error());
            } else {
                pending_messages = std::move(*follow_up);
            }
        }

        if (!has_more_tool_calls && pending_messages.empty()) {
            state.streaming_message.reset();
            CCH_TRY_VOID(emit_agent_end());
            sync_state(state, context);
            co_return AsyncAgentRunResult{std::move(context), assistant->stop_reason, turn, std::move(state)};
        }
    }

    // Reachable only when an explicit host-set turn cap is configured: the
    // uncapped default never exhausts the loop (ADR 0015). Exhaustion of the
    // host's own configured budget is a validation-classified outcome, never
    // a provider error.
    auto error = util::make_error(
        util::ErrorCode::Validation,
        "max turns exceeded",
        "agent reached the configured max_turns before a final assistant response");
    CCH_TRY_VOID(emit_agent_end());
    co_return std::unexpected(error);
}

} // namespace cch::agent
