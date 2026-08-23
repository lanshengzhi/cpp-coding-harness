#include "agent/AgentExecution.hpp"

#include "agent/ExecutionShared.hpp"
#include "agent/ToolCallExecutor.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"
#include <cch/ai/Content.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent::detail {
namespace {

struct StreamEventState {
    bool assistant_start_emitted{false};
};

[[nodiscard]] ai::AiContext context_from_snapshot(AgentExecutionSnapshot snapshot) {
    ai::AiContext context;
    context.system_prompt = std::move(snapshot.system_prompt);
    context.messages = std::move(snapshot.messages);
    context.tools = std::move(snapshot.tools);
    return context;
}

[[nodiscard]] support::ExpectedVoid append_message_with_lifecycle(
        ai::AiContext& context, AgentEventSink& emit, ai::MessageVariant message) {
    if (auto result = emit_agent_event(emit, MessageStartEvent{message}); !result) {
        return result;
    }
    context.messages.push_back(std::move(message));
    return emit_agent_event(emit, MessageEndEvent{context.messages.back()});
}

/// Per-turn `reasoning` streamSimple option (pi `agent-harness.ts`
/// `createStreamFn` forwards `streamOptions.reasoning`, which
/// `createLoopConfig` derives from the thinking level: `off` → undefined,
/// otherwise the level). Empty or `off` forwards no reasoning; the other six
/// levels map to the stream `ThinkingLevel`.
[[nodiscard]] std::optional<ai::ThinkingLevel> stream_reasoning(std::string_view level) {
    if (level.empty() || level == "off") {
        return std::nullopt;
    }
    if (level == "minimal") {
        return ai::ThinkingLevel::Minimal;
    }
    if (level == "low") {
        return ai::ThinkingLevel::Low;
    }
    if (level == "medium") {
        return ai::ThinkingLevel::Medium;
    }
    if (level == "high") {
        return ai::ThinkingLevel::High;
    }
    if (level == "xhigh") {
        return ai::ThinkingLevel::XHigh;
    }
    if (level == "max") {
        return ai::ThinkingLevel::Max;
    }
    return std::nullopt;
}

} // namespace

boost::asio::awaitable<support::ExpectedVoid> run_agent_execution(RunPolicy& policy,
        AgentExecutionCallbacks callbacks,
        std::optional<ai::UserMessage> user_message,
        std::stop_token stop_token) {
    auto initial_snapshot = callbacks.snapshot();
    if (!user_message && initial_snapshot.messages.empty()) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Cannot continue: no messages in context"));
    }
    if (!user_message && std::holds_alternative<ai::AssistantMessage>(initial_snapshot.messages.back())) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Cannot continue from message role: assistant"));
    }

    ai::Model model;
    std::string thinking_level;
    ai::AiContext context;
    bool context_replaced = false;

    auto emit = std::make_shared<AgentEventSink>(std::move(callbacks.emit));
    std::vector<ai::MessageVariant> new_messages;
    const auto emit_agent_end = [emit, &new_messages]() -> support::ExpectedVoid {
        return emit_agent_event(*emit, AgentEndEvent{new_messages});
    };
    CCH_TRY_VOID(emit_agent_event(*emit, AgentStartEvent{}));

    const bool has_initial_user_message = user_message.has_value();
    ai::MessageVariant initial_user_message;
    if (user_message) {
        initial_user_message = ai::MessageVariant{std::move(*user_message)};
    }

    std::vector<ai::MessageVariant> pending_messages;
    bool cancellation_completion_attempted = false;

    for (int turn = 1; !policy.max_turns || turn <= *policy.max_turns ||
                       (stop_token.stop_requested() && !cancellation_completion_attempted);
            ++turn) {
        CCH_TRY_VOID(emit_agent_event(*emit, TurnStartEvent{}));
        auto turn_snapshot = callbacks.snapshot();
        model = turn_snapshot.model;
        thinking_level = turn_snapshot.thinking_level;
        if (!context_replaced || turn == 1) {
            context = context_from_snapshot(std::move(turn_snapshot));
        } else {
            context.tools = std::move(turn_snapshot.tools);
        }

        if (turn == 1 && has_initial_user_message) {
            CCH_TRY_VOID(append_message_with_lifecycle(context, *emit, std::move(initial_user_message)));
            new_messages.push_back(context.messages.back());
        }

        if (turn == 1) {
            pending_messages = callbacks.drain(InputQueueKind::Steering);
        }

        if (!pending_messages.empty()) {
            for (auto& message : pending_messages) {
                CCH_TRY_VOID(append_message_with_lifecycle(context, *emit, std::move(message)));
                new_messages.push_back(context.messages.back());
            }
            pending_messages.clear();
        }

        ai::SimpleStreamOptions stream_options;
        // Every turn forwards the active prompt cancellation signal, the
        // harness-consumer session id, and the remaining harness-consumer
        // option set exactly as pi's `agent-harness.ts` `createStreamFn` does:
        // `reasoning` (off → undefined), `cacheRetention` (unset so the
        // pi-aligned default "short" applies; ADR 0033: compaction is the
        // only agent-core consumer that overrides it, with "none" and a fresh
        // session id), `timeoutMs`, `maxRetries`, `maxRetryDelayMs`, and
        // `headers`. `transport` stays fixed per adapter and
        // `metadata`/`onPayload`/`onResponse`/`thinkingBudgets` stay omitted
        // per #329 — none of them exist on the frozen `SimpleStreamOptions`
        // caller surface.
        stream_options.stop_token = stop_token;
        if (!policy.session_id.empty()) {
            stream_options.session_id = policy.session_id;
        }
        stream_options.reasoning = stream_reasoning(thinking_level);
        stream_options.cache_retention = policy.cache_retention;
        stream_options.timeout_ms = policy.timeout_ms;
        stream_options.max_retries = policy.max_retries;
        stream_options.max_retry_delay_ms = policy.max_retry_delay_ms;
        stream_options.headers = policy.headers;

        ai::AiContext request_context = context;
        {
            bool transform_cancelled = false;
            if (policy.transform_context) {
                auto transformed = co_await invoke_agent_hook(
                        "transformContext", *policy.transform_context, std::move(request_context.messages), stop_token);
                if (!transformed) {
                    if (!stop_token.stop_requested()) {
                        CCH_TRY_VOID(emit_agent_end());
                        co_return std::unexpected(transformed.error());
                    }
                    // A signal-aware transform may settle its own cancellation
                    // through the hook error value. Keep cancellation on the
                    // ordinary provider path so it can produce the one
                    // authoritative aborted Assistant Message.
                    request_context = context;
                    transform_cancelled = true;
                } else {
                    request_context.messages = std::move(*transformed);
                }
            }
            if (!transform_cancelled && policy.convert_to_llm) {
                auto converted = co_await invoke_agent_hook(
                        "convertToLlm", *policy.convert_to_llm, std::move(request_context.messages));
                if (!converted) {
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(converted.error());
                }
                if (converted->empty()) {
                    auto error = support::make_error(support::ErrorCode::Validation,
                            "convertToLlm returned no messages",
                            "LLM request would be empty");
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(error);
                }
                request_context.messages = std::move(*converted);
            }
        }

        // Tracks whether the provider emitted an assistant start event for
        // this response, so a terminal-before-start sequence can be recovered
        // with one synthesized start (matching pi's addedPartial rule).
        auto stream_event_state = std::make_shared<StreamEventState>();
        if (stop_token.stop_requested()) {
            cancellation_completion_attempted = true;
        }
        auto stream = policy.stream_factory(model, std::move(request_context), std::move(stream_options));
        auto assistant = co_await ai::detail::await_async_result(std::move(stream).run(
                [emit, stream_event_state](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
                    if (const auto* start = std::get_if<ai::AssistantStartEvent>(&event)) {
                        if (stream_event_state->assistant_start_emitted) {
                            return {};
                        }
                        stream_event_state->assistant_start_emitted = true;
                        return emit_agent_event(*emit, MessageStartEvent{ai::MessageVariant{start->partial}});
                    }
                    if (const auto* start = std::get_if<ai::TextStartEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{start->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{delta->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* end = std::get_if<ai::TextEndEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{end->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* start = std::get_if<ai::ThinkingStartEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{start->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* delta = std::get_if<ai::ThinkingDeltaEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{delta->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* end = std::get_if<ai::ThinkingEndEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{end->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* start = std::get_if<ai::ToolCallStartEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{start->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* delta = std::get_if<ai::ToolCallDeltaEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{delta->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (const auto* end = std::get_if<ai::ToolCallEndEvent>(&event)) {
                        return emit_agent_event(*emit,
                                MessageUpdateEvent{
                                        .message = ai::MessageVariant{end->partial},
                                        .assistant_event = event,
                                });
                    }
                    if (std::holds_alternative<ai::AssistantDoneEvent>(event) ||
                            std::holds_alternative<ai::AssistantErrorEvent>(event)) {
                        return {};
                    }
                    return {};
                }));

        if (!assistant) {
            CCH_TRY_VOID(emit_agent_end());
            co_return std::unexpected(assistant.error());
        }

        context.messages.push_back(ai::MessageVariant{*assistant});
        new_messages.push_back(context.messages.back());
        if (!stream_event_state->assistant_start_emitted) {
            // A conforming host provider may reach its terminal event before
            // any assistant start event. Synthesize the missing start from the
            // authoritative final message so consumers observe exactly one
            // complete assistant lifecycle.
            CCH_TRY_VOID(emit_agent_event(*emit, MessageStartEvent{context.messages.back()}));
        }
        CCH_TRY_VOID(emit_agent_event(*emit, MessageEndEvent{context.messages.back()}));

        if (assistant->stop_reason == ai::AssistantStopReason::Error ||
                assistant->stop_reason == ai::AssistantStopReason::Aborted) {
            CCH_TRY_VOID(emit_agent_event(*emit,
                    TurnEndEvent{
                            .message = ai::MessageVariant{*assistant},
                            .tool_results = {},
                    }));
            CCH_TRY_VOID(emit_agent_end());
            co_return support::ExpectedVoid{};
        }

        auto calls = tool_calls_from(*assistant);
        std::vector<ai::ToolResultMessage> tool_results;
        bool terminate_batch = false;
        if (!calls.empty() && assistant->stop_reason == ai::AssistantStopReason::Length) {
            tool_results.reserve(calls.size());
            for (const auto& call : calls) {
                CCH_TRY_VOID(emit_agent_event(*emit,
                        ToolExecutionStartEvent{
                                .tool_call_id = call.id,
                                .tool_name = call.name,
                                .args = call.arguments.value_or(support::JsonValue{}),
                        }));

                ai::ToolResultMessage result;
                result.tool_call_id = call.id;
                result.tool_name = call.name;
                result.is_error = true;
                result.content.emplace_back(ai::text_content(
                        "Tool call \"" + call.name +
                        "\" was not executed: the response hit the output token limit, so its "
                        "arguments may be truncated. Re-issue the tool call with complete arguments."));

                AsyncToolExecutionResult execution_result;
                execution_result.content = result.content;
                execution_result.is_error = true;
                CCH_TRY_VOID(emit_agent_event(*emit,
                        ToolExecutionEndEvent{
                                .tool_call_id = call.id,
                                .tool_name = call.name,
                                .result = std::move(execution_result),
                                .is_error = true,
                        }));
                CCH_TRY_VOID(emit_tool_result_message(*emit, result));
                tool_results.push_back(std::move(result));
            }
        } else if (!calls.empty()) {
            ToolCallExecutorOptions executor_options;
            executor_options.before_tool_call = policy.before_tool_call ? &*policy.before_tool_call : nullptr;
            executor_options.after_tool_call = policy.after_tool_call ? &*policy.after_tool_call : nullptr;
            executor_options.stop_token = stop_token;
            executor_options.execution = policy.tool_execution;
            ToolCallExecutor executor{policy.registry, std::move(executor_options)};

            auto execution = co_await executor.execute(ToolCallBatchRequest{*assistant, context}, *emit);
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
        }

        CCH_TRY_VOID(emit_agent_event(*emit,
                TurnEndEvent{
                        .message = ai::MessageVariant{*assistant},
                        .tool_results = tool_results,
                }));

        PrepareNextTurnContext next_turn_context;
        next_turn_context.assistant_message = *assistant;
        next_turn_context.tool_results = tool_results;
        next_turn_context.context = context;
        next_turn_context.new_messages = new_messages;

        if (!stop_token.stop_requested() && policy.prepare_next_turn) {
            auto update = co_await invoke_agent_hook("prepareNextTurn", *policy.prepare_next_turn, next_turn_context);
            if (!update) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(update.error());
            }
            if (*update) {
                if ((**update).model && !policy.validate_turn_update) {
                    auto error = support::make_error(
                            support::ErrorCode::Validation, "model update requires validation", (**update).model->id);
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(error);
                }
                if (policy.validate_turn_update) {
                    auto validated =
                            co_await invoke_agent_hook("validateTurnUpdate", *policy.validate_turn_update, **update);
                    if (!validated) {
                        CCH_TRY_VOID(emit_agent_end());
                        co_return std::unexpected(validated.error());
                    }
                }
                const bool replaced_context = (**update).context.has_value();
                auto applied = callbacks.apply_update(std::move(**update));
                if (!applied) {
                    CCH_TRY_VOID(emit_agent_end());
                    co_return std::unexpected(applied.error());
                }
                model = applied->model;
                thinking_level = applied->thinking_level;
                if (replaced_context) {
                    context_replaced = true;
                    context = context_from_snapshot(std::move(*applied));
                } else {
                    if (!context_replaced) {
                        context.system_prompt = std::move(applied->system_prompt);
                    }
                    context.tools = std::move(applied->tools);
                }
            }
        }

        next_turn_context.context = context;
        if (!stop_token.stop_requested() && policy.should_stop_after_turn) {
            auto should_stop = co_await invoke_agent_hook(
                    "shouldStopAfterTurn", *policy.should_stop_after_turn, next_turn_context);
            if (!should_stop) {
                CCH_TRY_VOID(emit_agent_end());
                co_return std::unexpected(should_stop.error());
            }
            if (*should_stop) {
                CCH_TRY_VOID(emit_agent_end());
                co_return support::ExpectedVoid{};
            }
        }

        if (!stop_token.stop_requested()) {
            pending_messages = callbacks.drain(InputQueueKind::Steering);
        }

        const bool has_more_tool_calls = !calls.empty() && (!terminate_batch || stop_token.stop_requested());
        if (!stop_token.stop_requested() && !has_more_tool_calls && pending_messages.empty()) {
            pending_messages = callbacks.drain(InputQueueKind::FollowUp);
        }

        if (!has_more_tool_calls && pending_messages.empty()) {
            CCH_TRY_VOID(emit_agent_end());
            co_return support::ExpectedVoid{};
        }
    }

    // Reachable only when an explicit host-set turn cap is configured: the
    // uncapped default never exhausts the loop (ADR 0015). Exhaustion of the
    // host's own configured budget is a validation-classified outcome, never
    // a provider error.
    auto error = support::make_error(support::ErrorCode::Validation,
            "max turns exceeded",
            "agent reached the configured max_turns before a final assistant response");
    CCH_TRY_VOID(emit_agent_end());
    co_return std::unexpected(error);
}

} // namespace cch::agent::detail
