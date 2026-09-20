#include "agent/AgentImpl.hpp"

#include "agent/ToolCallExecutor.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"

#include <memory>
#include <type_traits>
#include <optional>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {

[[nodiscard]] support::ExpectedVoid Agent::Impl::process_event(const AgentLifecycleEvent& event,
        AgentEventCommitter& commitment,
        std::optional<support::Error>& commitment_failure) {
    reduce_state(event);

    std::optional<AgentLifecycleEvent> invocation_event;
    const AgentLifecycleEvent* delivered_event = &event;
    if (std::holds_alternative<AgentEndEvent>(event)) {
        AgentEndEvent invocation_end;
        invocation_end.messages = invocation_messages();
        invocation_event.emplace(std::move(invocation_end));
        delivered_event = &*invocation_event;
    }

    // Weak observers cannot veto progress. The separately named strong
    // commitment runs only after state and every observer saw the event.
    // Each delivery turn snapshots the live registry so a reentrant
    // subscribe/unsubscribe cannot invalidate this pass; a subscriber
    // added now first runs on the next event.
    const auto delivery_snapshot = subscribers;
    (void)notify(*delivered_event, delivery_snapshot);
    if (!commitment) {
        return {};
    }
    auto committed = commitment(*delivered_event);
    if (!committed && !commitment_failure) {
        commitment_failure = committed.error();
    }
    return committed;
}

void Agent::Impl::reduce_state(const AgentLifecycleEvent& event) {
    // Idempotent pending-id admission shared by the update, end, and start
    // arms; arms keep their own empty-id gate exactly as before.
    const auto track_pending_tool_call = [this](const std::string& tool_call_id) {
        if (std::find(state.pending_tool_call_ids.begin(), state.pending_tool_call_ids.end(), tool_call_id) ==
                state.pending_tool_call_ids.end()) {
            state.pending_tool_call_ids.push_back(tool_call_id);
        }
    };

    if (const auto* start = std::get_if<MessageStartEvent>(&event)) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&start->message)) {
            state.streaming_message = *assistant;
        }
        return;
    }
    if (const auto* update = std::get_if<MessageUpdateEvent>(&event)) {
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&update->message)) {
            state.streaming_message = *assistant;
        }
        if (const auto* tool_end = std::get_if<ai::ToolCallEndEvent>(&update->assistant_event)) {
            if (!tool_end->tool_call.id.empty()) {
                track_pending_tool_call(tool_end->tool_call.id);
            }
        }
        return;
    }
    if (const auto* end = std::get_if<MessageEndEvent>(&event)) {
        state.messages.push_back(end->message);
        if (const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message)) {
            state.streaming_message.reset();
            state.pending_tool_call_ids.clear();
            for (const auto& block : assistant->content) {
                if (const auto* call = std::get_if<ai::ToolCallContent>(&block); call != nullptr && !call->id.empty()) {
                    track_pending_tool_call(call->id);
                }
            }
        }
        return;
    }
    if (const auto* start = std::get_if<ToolExecutionStartEvent>(&event)) {
        track_pending_tool_call(start->tool_call_id);
        return;
    }
    if (const auto* end = std::get_if<ToolExecutionEndEvent>(&event)) {
        std::erase(state.pending_tool_call_ids, end->tool_call_id);
        return;
    }
    if (std::holds_alternative<TurnEndEvent>(event)) {
        state.pending_tool_call_ids.clear();
        return;
    }
    if (std::holds_alternative<AgentEndEvent>(event)) {
        state.streaming_message.reset();
    }
}

/// Shared execution body for `prompt` and `continue_run`: installs the
/// run-stop source, settles run state on every exit path, and drives the
/// turn state machine. All live Agent state is reduced or applied in this
/// implementation object; the turn machine works on per-turn copies.
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> Agent::Impl::run_loop(std::shared_ptr<Impl> impl,
        std::optional<ai::UserMessage> user_message,
        AgentEventCommitter commitment,
        std::stop_source stop_source) {
    impl->active_run = true;
    impl->active_stop_source.emplace(std::move(stop_source));
    impl->state.is_running = true;
    impl->state.streaming_message.reset();
    impl->state.pending_tool_call_ids.clear();
    impl->invocation_message_offset = impl->state.messages.size();

    const auto finish_run = [impl] {
        impl->state.streaming_message.reset();
        impl->state.pending_tool_call_ids.clear();
        impl->state.is_running = false;
        impl->active_stop_source.reset();
        impl->active_run = false;
        impl->remove_unregistered_subscribers();
    };

    auto commitment_state = std::make_shared<CommitmentState>(CommitmentState{.commitment = std::move(commitment)});
    auto result =
            co_await run_turns(impl, commitment_state, std::move(user_message), impl->active_stop_source->get_token());
    finish_run();

    if (commitment_state->failure) {
        co_return std::unexpected(std::move(*commitment_state->failure));
    }
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return support::ExpectedVoid{};
}

boost::asio::awaitable<support::ExpectedVoid> Agent::Impl::run_turns(std::shared_ptr<Impl> impl,
        std::shared_ptr<CommitmentState> commitment_state,
        std::optional<ai::UserMessage> user_message,
        std::stop_token stop_token) {
    RunPolicy& policy = impl->run_policy;
    auto initial_snapshot = impl->snapshot();
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

    auto emit = std::make_shared<AgentEventSink>([impl, commitment_state](const AgentLifecycleEvent& event) {
        return impl->process_event(event, commitment_state->commitment, commitment_state->failure);
    });
    // The delivered AgentEndEvent payload is filled by process_event from the
    // invocation slice: the single authority for the messages produced by this
    // invocation.
    const auto emit_agent_end = [emit]() -> support::ExpectedVoid { return emit_agent_event(*emit, AgentEndEvent{}); };
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
        auto turn_snapshot = impl->snapshot();
        model = turn_snapshot.model;
        thinking_level = turn_snapshot.thinking_level;
        if (!context_replaced || turn == 1) {
            context = context_from_snapshot(std::move(turn_snapshot));
        } else {
            context.tools = std::move(turn_snapshot.tools);
        }

        if (turn == 1 && has_initial_user_message) {
            CCH_TRY_VOID(append_message_with_lifecycle(context, *emit, std::move(initial_user_message)));
        }

        if (turn == 1) {
            pending_messages = impl->drain(InputQueueKind::Steering);
        }

        if (!pending_messages.empty()) {
            for (auto& message : pending_messages) {
                CCH_TRY_VOID(append_message_with_lifecycle(context, *emit, std::move(message)));
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
        stream_options.reasoning = ai::parse_stream_thinking_level(thinking_level);
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
        auto assistant = co_await support::detail::await_async_result(std::move(stream).run(
                [emit, stream_event_state](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
                    return std::visit(
                            [&](const auto& concrete) -> support::ExpectedVoid {
                                using Event = std::decay_t<decltype(concrete)>;
                                // The message lifecycle start carries the
                                // dedup guard; every other partial-bearing
                                // event maps one-to-one onto the same
                                // MessageUpdateEvent shape.
                                if constexpr (std::is_same_v<Event, ai::AssistantStartEvent>) {
                                    if (stream_event_state->assistant_start_emitted) {
                                        return {};
                                    }
                                    stream_event_state->assistant_start_emitted = true;
                                    return emit_agent_event(
                                            *emit, MessageStartEvent{ai::MessageVariant{concrete.partial}});
                                } else if constexpr (std::is_same_v<Event, ai::AssistantErrorEvent>) {
                                    stream_event_state->inference_failure = concrete.inference_failure;
                                    return {};
                                } else if constexpr (std::is_same_v<Event, ai::AssistantDoneEvent>) {
                                    return {};
                                } else if constexpr (requires { concrete.partial; }) {
                                    return emit_agent_event(*emit,
                                            MessageUpdateEvent{
                                                    .message = ai::MessageVariant{concrete.partial},
                                                    .assistant_event = event,
                                            });
                                } else {
                                    return {};
                                }
                            },
                            event);
                }));

        if (!assistant) {
            CCH_TRY_VOID(emit_agent_end());
            co_return std::unexpected(assistant.error());
        }

        context.messages.push_back(ai::MessageVariant{*assistant});
        if (!stream_event_state->assistant_start_emitted) {
            // A conforming host provider may reach its terminal event before
            // any assistant start event. Synthesize the missing start from the
            // authoritative final message so consumers observe exactly one
            // complete assistant lifecycle.
            CCH_TRY_VOID(emit_agent_event(*emit, MessageStartEvent{context.messages.back()}));
        }
        CCH_TRY_VOID(emit_agent_event(*emit,
                MessageEndEvent{
                        .message = context.messages.back(),
                        .inference_failure = stream_event_state->inference_failure,
                }));

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
        next_turn_context.new_messages = impl->invocation_messages();

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
                auto applied = impl->apply_update(std::move(**update));
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
            pending_messages = impl->drain(InputQueueKind::Steering);
        }

        const bool has_more_tool_calls = !calls.empty() && (!terminate_batch || stop_token.stop_requested());
        if (!stop_token.stop_requested() && !has_more_tool_calls && pending_messages.empty()) {
            pending_messages = impl->drain(InputQueueKind::FollowUp);
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

} // namespace cch::agent
