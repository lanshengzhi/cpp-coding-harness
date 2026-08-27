#include <cch/agent/Agent.hpp>

#include "agent/AgentMessageAccess.hpp"
#include "agent/ExecutionShared.hpp"
#include "agent/ToolCallExecutor.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Model.hpp>
#include "support/BoundedText.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#include <exception>
#endif
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

struct AgentSubscriptionAnchor {
    Agent::Impl* agent{nullptr};
};

[[nodiscard]] support::ExpectedVoid invoke_weak_observer(AgentEventSink& sink, const AgentLifecycleEvent& event) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        return sink(event);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& exception) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown, exception.what()));
    } catch (...) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown, "unknown exception"));
    }
#endif
}

std::vector<std::string> tool_names(const std::vector<ai::Tool>& definitions) {
    std::vector<std::string> names;
    names.reserve(definitions.size());
    for (const auto& definition : definitions) {
        names.push_back(definition.name);
    }
    return names;
}

[[nodiscard]] bool is_valid_thinking_level(std::string_view level) {
    static const std::vector<std::string> allowed{"off", "minimal", "low", "medium", "high", "xhigh", "max"};
    return std::find(allowed.begin(), allowed.end(), level) != allowed.end();
}

[[nodiscard]] std::size_t approximate_content_size(const ai::Content& block) {
    return std::visit(
            [](const auto& content) -> std::size_t {
                if constexpr (std::is_same_v<std::decay_t<decltype(content)>, ai::TextContent>) {
                    return content.text.size();
                } else if constexpr (std::is_same_v<std::decay_t<decltype(content)>, ai::ImageContent>) {
                    return content.data.size() + content.mime_type.size();
                } else if constexpr (std::is_same_v<std::decay_t<decltype(content)>, ai::ThinkingContent>) {
                    return content.thinking.size();
                }
                return 0;
            },
            block);
}

[[nodiscard]] std::size_t approximate_message_size(const ai::MessageVariant& message) {
    return std::visit(
            [](const auto& current) -> std::size_t {
                if constexpr (std::is_same_v<std::decay_t<decltype(current)>, ai::UserMessage>) {
                    std::size_t size = 0;
                    if (const auto* text = std::get_if<std::string>(&current.content)) {
                        size = text->size();
                    } else {
                        for (const auto& block : std::get<std::vector<ai::Content>>(current.content)) {
                            size += approximate_content_size(block);
                        }
                    }
                    return size;
                } else if constexpr (std::is_same_v<std::decay_t<decltype(current)>, ai::AssistantMessage>) {
                    std::size_t size = 0;
                    for (const auto& block : current.content) {
                        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                            size += text->text.size();
                        } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                            size += thinking->thinking.size();
                        } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                            size += call->raw_arguments.size();
                        }
                    }
                    return size;
                } else if constexpr (std::is_same_v<std::decay_t<decltype(current)>, ai::ToolResultMessage>) {
                    return ai::text_from_content(current.content).size();
                } else if constexpr (std::is_same_v<std::decay_t<decltype(current)>, ai::SystemMessage>) {
                    return current.content.size();
                }
                return 0;
            },
            message);
}

[[nodiscard]] support::ExpectedVoid admit_queued_message(
        AgentInputQueues& queues, AgentInputQueue& queue, ai::MessageVariant message, std::string_view queue_name) {
    const std::size_t message_bytes = approximate_message_size(message);
    if (queue.messages.size() + 1 > queues.max_messages) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "too many queued messages",
                std::string{queue_name} + " message count exceeds " + std::to_string(queues.max_messages)));
    }

    std::size_t queued_bytes = message_bytes;
    for (const auto& queued : queue.messages) {
        queued_bytes += approximate_message_size(queued);
    }
    if (queued_bytes > queues.max_bytes) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "queued messages too large",
                std::string{queue_name} + " message byte size exceeds " + std::to_string(queues.max_bytes)));
    }

    queue.messages.push_back(std::move(message));
    return {};
}

/// Input queue selector for the turn machine's drains.
enum class InputQueueKind { Steering, FollowUp };

/// Owned facts read from live Agent state at a turn boundary. The live
/// AgentState remains in Agent::Impl; this value is the turn machine's
/// per-turn working copy.
struct AgentExecutionSnapshot {
    ai::Model model;
    std::string thinking_level;
    std::string system_prompt;
    std::vector<ai::MessageVariant> messages;
    std::vector<ai::Tool> tools;
};

/// Run configuration that is not live Agent state. It is owned by Agent::Impl
/// and remains alive for every run started by that Agent.
struct RunPolicy {
    ai::ModelStreamFactory stream_factory;
    ToolRegistry registry;
    std::string session_id;
    std::optional<int> max_turns{std::nullopt};
    std::optional<ai::CacheRetention> cache_retention{std::nullopt};
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    std::uint32_t max_retries{0};
    std::optional<std::uint64_t> max_retry_delay_ms{std::nullopt};
    ai::RequestHeaders headers{};
    std::optional<BeforeToolCallHook> before_tool_call{std::nullopt};
    std::optional<AfterToolCallHook> after_tool_call{std::nullopt};
    std::optional<TransformContextHook> transform_context{std::nullopt};
    std::optional<ConvertToLlmHook> convert_to_llm{std::nullopt};
    std::optional<PrepareNextTurnHook> prepare_next_turn{std::nullopt};
    std::optional<ShouldStopAfterTurnHook> should_stop_after_turn{std::nullopt};
    std::optional<ValidateTurnUpdateHook> validate_turn_update{std::nullopt};
    ToolExecutionPolicy tool_execution{BoundedParallelToolExecution{}};
};

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

struct AgentEventSubscription::Impl {
    std::size_t id{};
    std::weak_ptr<AgentSubscriptionAnchor> anchor;
};

struct Agent::Impl {
    struct Subscriber {
        std::size_t id{};
        AgentEventSink sink;
        bool registered{true};
        bool delivery_enabled{true};
    };

    struct CommitmentState {
        AgentEventCommitter commitment;
        std::optional<support::Error> failure{std::nullopt};
    };

    Impl(ai::ModelStreamFactory stream_factory,
            std::vector<ai::Tool> definitions,
            ToolRegistry tools,
            AsyncAgentOptions options,
            AgentInitialState initial_state)
        : run_policy{.stream_factory = std::move(stream_factory),
                  .registry = std::move(tools),
                  .session_id = std::move(options.session_id),
                  .max_turns = options.max_turns,
                  .cache_retention = options.cache_retention,
                  .timeout_ms = options.timeout_ms,
                  .max_retries = options.max_retries,
                  .max_retry_delay_ms = options.max_retry_delay_ms,
                  .headers = std::move(options.headers),
                  .before_tool_call = std::move(options.before_tool_call),
                  .after_tool_call = std::move(options.after_tool_call),
                  .transform_context = std::move(options.transform_context),
                  .convert_to_llm = std::move(options.convert_to_llm),
                  .prepare_next_turn = std::move(options.prepare_next_turn),
                  .should_stop_after_turn = std::move(options.should_stop_after_turn),
                  .validate_turn_update = std::move(options.validate_turn_update),
                  .tool_execution = options.tool_execution} {
        state.messages = std::move(initial_state.messages);
        state.model = std::move(options.model);
        state.thinking_level = options.thinking_level.empty() ? "medium" : std::move(options.thinking_level);
        state.thinking_level = ai::clamp_thinking_level_string(state.model, state.thinking_level);
        state.system_prompt = std::move(options.system_prompt);
        state.input_queues.max_messages = options.max_queued_messages;
        state.input_queues.max_bytes = options.max_queued_bytes;
        state.input_queues.steering.mode = options.steering_mode;
        state.input_queues.follow_up.mode = options.follow_up_mode;
        state.active_tool_names = tool_names(definitions);
    }

    [[nodiscard]] AgentExecutionSnapshot snapshot() const {
        return AgentExecutionSnapshot{
                .model = state.model,
                .thinking_level = state.thinking_level,
                .system_prompt = state.system_prompt,
                .messages = state.messages,
                .tools = run_policy.registry.definitions(),
        };
    }

    [[nodiscard]] support::Expected<AgentExecutionSnapshot> apply_update(AgentLoopTurnUpdate update) {
        if (update.model) {
            if (auto valid = ai::validate_model(*update.model); !valid) {
                return std::unexpected(valid.error());
            }
        }
        if (update.thinking_level && !is_valid_thinking_level(*update.thinking_level)) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "invalid thinking level", *update.thinking_level));
        }

        ai::Model next_model = state.model;
        std::string next_thinking_level = state.thinking_level;
        if (update.model) {
            next_model = *update.model;
        }
        if (update.thinking_level) {
            next_thinking_level = *update.thinking_level;
        }
        next_thinking_level = ai::clamp_thinking_level_string(next_model, next_thinking_level);

        // A replacement is the next model-facing context, not a new durable
        // Agent state. Live history remains event-owned so AgentEnd still
        // reports the messages produced by this invocation.
        state.model = std::move(next_model);
        state.thinking_level = std::move(next_thinking_level);

        auto updated = snapshot();
        if (update.context) {
            updated.system_prompt = update.context->system_prompt.value_or("");
            updated.messages = std::move(update.context->messages);
        }
        return updated;
    }

    [[nodiscard]] std::vector<ai::MessageVariant> drain(InputQueueKind queue_kind) {
        auto& queue = queue_kind == InputQueueKind::Steering ? state.input_queues.steering.messages
                                                             : state.input_queues.follow_up.messages;
        const auto mode = queue_kind == InputQueueKind::Steering ? state.input_queues.steering.mode
                                                                 : state.input_queues.follow_up.mode;
        if (queue.empty()) {
            return {};
        }
        if (mode == InputQueueMode::All) {
            auto drained = std::move(queue);
            queue.clear();
            return drained;
        }
        std::vector<ai::MessageVariant> drained;
        drained.push_back(std::move(queue.front()));
        queue.erase(queue.begin());
        return drained;
    }

    /// The messages produced by this invocation: the live-history slice since
    /// run start. The single authority behind delivered `AgentEndEvent`
    /// payloads (see `process_event`) and `PrepareNextTurnContext.new_messages`.
    [[nodiscard]] std::vector<ai::MessageVariant> invocation_messages() const {
        return std::vector<ai::MessageVariant>(
                state.messages.begin() + static_cast<std::ptrdiff_t>(invocation_message_offset), state.messages.end());
    }

    [[nodiscard]] support::ExpectedVoid process_event(const AgentLifecycleEvent& event,
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

    void reduce_state(const AgentLifecycleEvent& event) {
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
                if (!tool_end->tool_call.id.empty() &&
                        std::find(state.pending_tool_call_ids.begin(),
                                state.pending_tool_call_ids.end(),
                                tool_end->tool_call.id) == state.pending_tool_call_ids.end()) {
                    state.pending_tool_call_ids.push_back(tool_end->tool_call.id);
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
                    if (const auto* call = std::get_if<ai::ToolCallContent>(&block);
                            call != nullptr && !call->id.empty()) {
                        state.pending_tool_call_ids.push_back(call->id);
                    }
                }
            }
            return;
        }
        if (const auto* start = std::get_if<ToolExecutionStartEvent>(&event)) {
            if (std::find(state.pending_tool_call_ids.begin(),
                        state.pending_tool_call_ids.end(),
                        start->tool_call_id) == state.pending_tool_call_ids.end()) {
                state.pending_tool_call_ids.push_back(start->tool_call_id);
            }
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

    void record_observer_diagnostic(const support::Error& failure) {
        constexpr std::size_t kMaxDiagnostics = 16;
        constexpr std::size_t kMaxDetailBytes = 1024;

        std::string detail = failure.message;
        if (!failure.detail.empty()) {
            detail += ": ";
            detail += failure.detail;
        }
        detail = support::bounded_redacted_text(std::move(detail), kMaxDetailBytes, "...");

        if (state.diagnostics.size() == kMaxDiagnostics) {
            state.diagnostics.erase(state.diagnostics.begin());
        }
        state.diagnostics.push_back(
                support::make_error(failure.code, "agent event observer failed", std::move(detail)));
    }

    [[nodiscard]] support::ExpectedVoid notify(
            const AgentLifecycleEvent& event, const std::vector<std::shared_ptr<Subscriber>>& delivery_snapshot) {
        for (const auto& subscriber : delivery_snapshot) {
            if (!subscriber->delivery_enabled || !subscriber->sink) {
                continue;
            }
            if (auto observed = invoke_weak_observer(subscriber->sink, event); !observed) {
                record_observer_diagnostic(observed.error());
                subscriber->registered = false;
                subscriber->delivery_enabled = false;
            }
        }
        if (!active_run) {
            remove_unregistered_subscribers();
        }
        return {};
    }

    void remove_unregistered_subscribers() {
        std::erase_if(
                subscribers, [](const std::shared_ptr<Subscriber>& subscriber) { return !subscriber->registered; });
    }

    void unsubscribe(std::size_t id) {
        for (const auto& subscriber : subscribers) {
            if (subscriber->id == id) {
                // Deactivate immediately: a delivery snapshot shares this
                // entry, and clearing delivery_enabled suppresses the
                // subscriber's turn for the current event and every later
                // event in the run.
                subscriber->registered = false;
                subscriber->delivery_enabled = false;
                break;
            }
        }
        // Keep deactivated entries reachable while a delivery snapshot may
        // still own them so a reentrant unsubscribe cannot invalidate
        // iteration.
        if (!active_run) {
            remove_unregistered_subscribers();
        }
    }

    void clear_subscriptions() {
        for (const auto& subscriber : subscribers) {
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        }
        subscribers.clear();
    }

    [[nodiscard]] bool is_subscribed(std::size_t id) const {
        return std::ranges::any_of(subscribers, [id](const std::shared_ptr<Subscriber>& subscriber) {
            return subscriber->id == id && subscriber->registered;
        });
    }

    /// Shared execution body for `prompt` and `continue_run`: installs the
    /// run-stop source, settles run state on every exit path, and drives the
    /// turn state machine. All live Agent state is reduced or applied in this
    /// implementation object; the turn machine works on per-turn copies.
    [[nodiscard]] static boost::asio::awaitable<support::ExpectedVoid> run_loop(std::shared_ptr<Impl> impl,
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
        auto result = co_await run_turns(
                impl, commitment_state, std::move(user_message), impl->active_stop_source->get_token());
        finish_run();

        if (commitment_state->failure) {
            co_return std::unexpected(std::move(*commitment_state->failure));
        }
        if (!result) {
            co_return std::unexpected(result.error());
        }
        co_return support::ExpectedVoid{};
    }

    /// The Agent Turn state machine (pi `agent-loop.ts` `runLoop`; lifecycle
    /// order per ADR 0014). Reads live state at each turn boundary through
    /// `snapshot`, applies turn updates through `apply_update`, drains input
    /// queues through `drain`, and emits every lifecycle event through
    /// `process_event` — state reduction first, weak observers second, the
    /// strong commitment last.
    [[nodiscard]] static boost::asio::awaitable<support::ExpectedVoid> run_turns(std::shared_ptr<Impl> impl,
            std::shared_ptr<CommitmentState> commitment_state,
            std::optional<ai::UserMessage> user_message,
            std::stop_token stop_token);

    RunPolicy run_policy;
    AgentState state;
    bool active_run{false};
    std::optional<std::stop_source> active_stop_source;
    std::size_t invocation_message_offset{};
    std::size_t next_subscriber_id{1};
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    std::shared_ptr<AgentSubscriptionAnchor> subscription_anchor;
};

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
        auto assistant = co_await support::detail::await_async_result(std::move(stream).run(
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

AgentEventSubscription::AgentEventSubscription(AgentEventSubscription&& other) noexcept = default;

AgentEventSubscription& AgentEventSubscription::operator=(AgentEventSubscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

AgentEventSubscription::~AgentEventSubscription() { unsubscribe(); }

void AgentEventSubscription::unsubscribe() {
    if (!impl_) {
        return;
    }
    if (auto anchor = impl_->anchor.lock(); anchor && anchor->agent) {
        anchor->agent->unsubscribe(impl_->id);
    }
    impl_.reset();
}

AgentEventSubscription::operator bool() const {
    if (!impl_) {
        return false;
    }
    const auto anchor = impl_->anchor.lock();
    return anchor && anchor->agent && anchor->agent->is_subscribed(impl_->id);
}

Agent::Agent(ai::ModelStreamFactory stream_factory,
        ToolRegistry tools,
        AsyncAgentOptions options,
        AgentInitialState initial_state) {
    options.thinking_level = initial_state.thinking_level;
    auto definitions = tools.definitions();
    impl_ = std::make_shared<Impl>(std::move(stream_factory),
            std::move(definitions),
            std::move(tools),
            std::move(options),
            std::move(initial_state));
    impl_->subscription_anchor = std::make_shared<AgentSubscriptionAnchor>(impl_.get());
}

Agent::Agent(Agent&&) noexcept = default;

Agent& Agent::operator=(Agent&& other) noexcept {
    if (this != &other) {
        if (impl_ && impl_->subscription_anchor) {
            impl_->subscription_anchor->agent = nullptr;
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Agent::~Agent() {
    if (impl_ && impl_->subscription_anchor) {
        impl_->subscription_anchor->agent = nullptr;
    }
}

support::AsyncResult<void> Agent::prompt(std::string user_prompt) { return prompt(std::move(user_prompt), {}); }

support::AsyncResult<void> Agent::prompt(std::string user_prompt, AgentEventCommitter commitment) {
    return prompt(ai::user_text_message(std::move(user_prompt)), std::move(commitment), std::stop_source{});
}

support::AsyncResult<void> Agent::prompt(
        ai::UserMessage user_message, AgentEventCommitter commitment, std::stop_source stop_source) {
    if (!impl_) {
        return support::AsyncResult<void>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"))};
    }
    if (impl_->active_run) {
        return support::AsyncResult<void>{std::unexpected(
                support::make_error(support::ErrorCode::Validation, "agent is busy (prompt already in flight)"))};
    }
    return support::detail::make_async_result(
            [impl = impl_,
                    user_message = std::move(user_message),
                    commitment = std::move(commitment),
                    stop_source = std::move(stop_source)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                if (!impl) {
                    co_return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
                }
                if (impl->active_run) {
                    co_return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "agent is busy (prompt already in flight)"));
                }
                co_return co_await Impl::run_loop(impl,
                        std::optional<ai::UserMessage>{std::move(user_message)},
                        std::move(commitment),
                        std::move(stop_source));
            });
}

support::AsyncResult<void> Agent::continue_run(AgentEventCommitter commitment, std::stop_source stop_source) {
    return support::detail::make_async_result(
            [impl = impl_,
                    commitment = std::move(commitment),
                    stop_source = std::move(stop_source)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                if (!impl) {
                    co_return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
                }
                if (impl->active_run) {
                    co_return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "agent is busy (prompt already in flight)"));
                }
                co_return co_await Impl::run_loop(impl, std::nullopt, std::move(commitment), std::move(stop_source));
            });
}

void Agent::abort() {
    if (impl_ && impl_->active_stop_source) {
        (void)impl_->active_stop_source->request_stop();
    }
}

support::ExpectedVoid Agent::steer(ai::MessageVariant message) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    auto& queues = impl_->state.input_queues;
    return admit_queued_message(queues, queues.steering, std::move(message), "steering");
}

support::ExpectedVoid Agent::follow_up(ai::MessageVariant message) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    auto& queues = impl_->state.input_queues;
    return admit_queued_message(queues, queues.follow_up, std::move(message), "follow-up");
}

support::ExpectedVoid Agent::set_steering_mode(InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    impl_->state.input_queues.steering.mode = mode;
    return {};
}

support::ExpectedVoid Agent::set_follow_up_mode(InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    impl_->state.input_queues.follow_up.mode = mode;
    return {};
}

support::Expected<std::string> Agent::set_thinking_level(std::string_view level) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    if (!is_valid_thinking_level(level)) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "invalid thinking level", std::string{level}));
    }
    // Clamp against the active model so an unsupported level can never be
    // forwarded to the stream (pi agent-session.ts setThinkingLevel clamps
    // before persisting; a clamped level equal to the current one is a no-op).
    const auto effective = ai::clamp_thinking_level_string(impl_->state.model, level);
    impl_->state.thinking_level = effective;
    return effective;
}

support::ExpectedVoid Agent::set_model(ai::Model model) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    if (auto valid = ai::validate_model(model); !valid) {
        return valid;
    }
    impl_->state.model = std::move(model);
    return {};
}

void Agent::set_system_prompt(std::string system_prompt) {
    if (!impl_) {
        return;
    }
    impl_->state.system_prompt = std::move(system_prompt);
}

support::ExpectedVoid Agent::clear_steering_queue() {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    impl_->state.input_queues.steering.messages.clear();
    return {};
}

support::ExpectedVoid Agent::clear_follow_up_queue() {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    impl_->state.input_queues.follow_up.messages.clear();
    return {};
}

support::ExpectedVoid Agent::clear_input_queues() {
    if (auto cleared = clear_steering_queue(); !cleared) {
        return cleared;
    }
    return clear_follow_up_queue();
}

AgentState Agent::state() const { return impl_ ? impl_->state : AgentState{}; }

support::Expected<AgentEventSubscription> Agent::subscribe(AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    if (!sink) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent event sink is empty"));
    }

    const auto id = impl_->next_subscriber_id++;
    impl_->subscribers.push_back(std::make_shared<Impl::Subscriber>(Impl::Subscriber{id, std::move(sink), true, true}));

    auto subscription_impl = std::make_unique<AgentEventSubscription::Impl>();
    subscription_impl->id = id;
    subscription_impl->anchor = impl_->subscription_anchor;

    AgentEventSubscription subscription;
    subscription.impl_ = std::move(subscription_impl);
    return subscription;
}

void Agent::clear_subscriptions() {
    if (impl_) {
        impl_->clear_subscriptions();
    }
}

support::ExpectedVoid detail::AgentMessageAccess::append_bash_execution(
        Agent& agent, ai::BashExecutionMessage message) {
    if (!agent.impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    if (agent.impl_->active_run) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "agent is busy (cannot commit passive message)"));
    }
    agent.impl_->state.messages.emplace_back(std::move(message));
    return {};
}

support::ExpectedVoid detail::AgentMessageAccess::replace_messages(
        Agent& agent, std::vector<ai::MessageVariant> messages) {
    if (!agent.impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    if (agent.impl_->active_run) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "agent is busy (cannot replace session context)"));
    }
    agent.impl_->state.messages = std::move(messages);
    agent.impl_->state.streaming_message.reset();
    agent.impl_->state.pending_tool_call_ids.clear();
    return {};
}

support::ExpectedVoid detail::AgentMessageAccess::pop_trailing_assistant(Agent& agent) {
    if (!agent.impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "agent is not initialized"));
    }
    if (agent.impl_->active_run) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "agent is busy (cannot mutate session context)"));
    }
    auto& messages = agent.impl_->state.messages;
    if (!messages.empty() && std::holds_alternative<ai::AssistantMessage>(messages.back())) {
        messages.pop_back();
    }
    return {};
}

} // namespace cch::agent
