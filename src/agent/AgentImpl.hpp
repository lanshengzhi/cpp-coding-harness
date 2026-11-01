#pragma once

#include <cch/agent/Agent.hpp>

#include "agent/ExecutionShared.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Model.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::agent {

/// The shared Impl state of one live Agent, held through its subscription
/// anchor so weak observers can reach the run machine during delivery.
struct AgentSubscriptionAnchor {
    Agent::Impl* agent{nullptr};
};

/// The not-initialized rejection shared by every public Agent operation.
[[nodiscard]] inline support::Error agent_not_initialized() {
    return support::make_error(support::ErrorCode::Validation, "agent is not initialized");
}

/// The "prompt run already in flight" rejection (pi `Agent.prompt`:
/// "Agent is already processing a prompt. Use steer() or followUp() to queue
/// messages, or wait for completion.").
[[nodiscard]] inline support::Error agent_busy_error() {
    return support::make_error(support::ErrorCode::Validation,
            "Agent is already processing a prompt. Use steer() or followUp() to queue messages, or wait for "
            "completion.");
}

/// The "continuation run already in flight" rejection (pi `Agent.continue`:
/// "Agent is already processing. Wait for completion before continuing.").
[[nodiscard]] inline support::Error agent_continue_busy_error() {
    return support::make_error(
            support::ErrorCode::Validation, "Agent is already processing. Wait for completion before continuing.");
}

/// The "reset during a run" rejection (pi `Agent.reset`: "Agent is already
/// processing. Wait for completion before resetting.").
[[nodiscard]] inline support::Error agent_reset_busy_error() {
    return support::make_error(
            support::ErrorCode::Validation, "Agent is already processing. Wait for completion before resetting.");
}

/// The continuation rejection for an empty or system-only transcript. The
/// public path checks it before draining queued input; the private session
/// path checks it in the turn machine.
[[nodiscard]] inline support::Error continuation_no_messages_error() {
    return support::make_error(support::ErrorCode::Validation, "Cannot continue: no messages in context");
}

/// The continuation rejection for an assistant-terminal transcript with no
/// queued input to fall back to.
[[nodiscard]] inline support::Error continuation_from_assistant_error() {
    return support::make_error(support::ErrorCode::Validation, "Cannot continue from message role: assistant");
}

/// The host-configured turn cap exhaustion, shared by the turn machine's
/// post-loop exit and the public continuation's pre-drain admission check.
[[nodiscard]] inline support::Error max_turns_exceeded_error() {
    return support::make_error(support::ErrorCode::Validation,
            "max turns exceeded",
            "agent reached the configured max_turns before a final assistant response");
}

[[nodiscard]] inline std::vector<std::string> tool_names(const std::vector<ai::Tool>& definitions) {
    std::vector<std::string> names;
    names.reserve(definitions.size());
    for (const auto& definition : definitions) {
        names.push_back(definition.name);
    }
    return names;
}

[[nodiscard]] inline bool is_valid_thinking_level(std::string_view level) {
    return ai::parse_model_thinking_level(level).has_value();
}

[[nodiscard]] inline std::size_t queued_selection_count(const AgentInputQueue& queue) {
    if (queue.messages.empty()) {
        return 0;
    }
    return queue.mode == InputQueueMode::All ? queue.messages.size() : 1;
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

/// The first turn's seed input for one run: messages carried into turn 1 plus
/// whether the caller already drained steering input, so the turn machine must
/// not poll the steering queue again at turn 1.
struct InitialInput {
    /// Messages committed ahead of the run's own user message (pi
    /// `_prepareRun`'s `messages.unshift(updateMessage)`): the Session's
    /// prompt-time system message carries the section diff the model has not
    /// seen yet, and `declare_tool_changes` anchors the loadout delta on it
    /// instead of declaring a second system message.
    std::vector<ai::MessageVariant> prefix_messages;
    std::vector<ai::MessageVariant> messages;
    bool steering_already_drained{false};
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
    std::optional<PrepareRequestHook> prepare_request{std::nullopt};
    std::optional<PrepareNextTurnHook> prepare_next_turn{std::nullopt};
    std::optional<FinishTurnHook> finish_turn{std::nullopt};
    std::optional<ValidateTurnUpdateHook> validate_turn_update{std::nullopt};
    ToolExecutionPolicy tool_execution{BoundedParallelToolExecution{}};
};

/// Per-run provider stream event facts that survive between stream callbacks.
struct StreamEventState {
    bool assistant_start_emitted{false};
    std::optional<ai::InferenceFailure> inference_failure{std::nullopt};
};

[[nodiscard]] inline ai::AiContext context_from_snapshot(AgentExecutionSnapshot snapshot) {
    ai::AiContext context;
    context.system_prompt = std::move(snapshot.system_prompt);
    context.messages = std::move(snapshot.messages);
    context.tools = std::move(snapshot.tools);
    return context;
}

/// The turn machine's working request state: the model, thinking level and
/// model-facing context the next provider request uses, plus whether a policy
/// replaced that context during the run.
struct TurnRequestState {
    ai::Model model{};
    std::string thinking_level;
    ai::AiContext context{};
    bool context_replaced{false};
};

/// Fold one applied request update into the turn machine's working state. A
/// replacement context becomes the model-facing context for the rest of the
/// run; otherwise only the system prompt (until a replacement exists) and the
/// live executable tool set advance.
[[nodiscard]] inline TurnRequestState adopt_turn_update(
        TurnRequestState state, AgentExecutionSnapshot applied, bool replaced_context) {
    state.model = std::move(applied.model);
    state.thinking_level = std::move(applied.thinking_level);
    if (replaced_context) {
        state.context_replaced = true;
        state.context = context_from_snapshot(std::move(applied));
        return state;
    }
    if (!state.context_replaced) {
        state.context.system_prompt = std::move(applied.system_prompt);
    }
    state.context.tools = std::move(applied.tools);
    return state;
}

[[nodiscard]] inline support::ExpectedVoid append_message_with_lifecycle(
        ai::AiContext& context, AgentEventSink& emit, ai::MessageVariant message) {
    if (auto result = emit_agent_event(emit, MessageStartEvent{message}); !result) {
        return result;
    }
    context.messages.push_back(std::move(message));
    return emit_agent_event(emit, MessageEndEvent{context.messages.back()});
}

/// Invoke one weak observer delivery directly (no extra failure wrapping).
[[nodiscard]] inline support::ExpectedVoid invoke_weak_observer(
        AgentEventSink& sink, const AgentLifecycleEvent& event) {
    return sink(event);
}

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
            AgentInitialState initial_state);

    [[nodiscard]] AgentExecutionSnapshot snapshot() const;

    [[nodiscard]] support::Expected<AgentExecutionSnapshot> apply_update(AgentRequestUpdate update);

    /// Validate one policy-supplied request update and apply it to live state.
    /// The coroutine owns `impl`, so its `run_policy` and validator outlive
    /// every suspension.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<AgentExecutionSnapshot>> validate_and_apply_update(
            std::shared_ptr<Impl> impl, AgentRequestUpdate update);

    /// Invoke the pi `finishTurn` policy for one completed turn. A missing
    /// policy is an empty decision; the caller owns `turn_end` and the
    /// decision's effect. The coroutine owns `impl`, so its `run_policy`
    /// outlives every suspension.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::optional<AgentTurnAction>>> invoke_finish_turn(
            std::shared_ptr<Impl> impl, AgentTurnContext turn);

    [[nodiscard]] std::vector<ai::MessageVariant> drain(InputQueueKind queue_kind);

    /// The messages produced by this invocation: the live-history slice since
    /// run start. The single authority behind delivered `AgentEndEvent`
    /// payloads (see `process_event`) and `AgentTurnContext.new_messages`.
    [[nodiscard]] std::vector<ai::MessageVariant> invocation_messages() const {
        return std::vector<ai::MessageVariant>(
                state.messages.begin() + static_cast<std::ptrdiff_t>(invocation_message_offset), state.messages.end());
    }

    /// State reduction, observer delivery, and the strong commitment for one
    /// lifecycle event, in that order (ADR 0014).
    [[nodiscard]] support::ExpectedVoid process_event(const AgentLifecycleEvent& event,
            AgentEventCommitter& commitment,
            std::optional<support::Error>& commitment_failure);

    void reduce_state(const AgentLifecycleEvent& event);

    void record_observer_diagnostic(const support::Error& failure);

    [[nodiscard]] std::uint64_t observer_diagnostic_serial() const { return observer_diagnostic_serial_; }

    /// Deliver one event to a delivery snapshot. Reported observer failures
    /// deactivate the subscriber without vetoing progress.
    [[nodiscard]] support::ExpectedVoid notify(
            const AgentLifecycleEvent& event, const std::vector<std::shared_ptr<Subscriber>>& delivery_snapshot);

    void remove_unregistered_subscribers();

    void unsubscribe(std::size_t id);

    void clear_subscriptions();

    [[nodiscard]] bool is_subscribed(std::size_t id) const;

    /// Shared execution body for `prompt` and `continue_run`: installs the
    /// run-stop source, settles run state on every exit path, and drives the
    /// turn state machine. All live Agent state is reduced or applied in this
    /// implementation object; the turn machine works on per-turn copies.
    [[nodiscard]] static boost::asio::awaitable<support::ExpectedVoid> run_loop(std::shared_ptr<Impl> impl,
            std::optional<ai::UserMessage> user_message,
            AgentEventCommitter commitment,
            std::stop_source stop_source,
            InitialInput initial_input);

    /// The Agent Turn state machine (pi `agent-loop.ts` `runLoop`; lifecycle
    /// order per ADR 0014). Reads live state at each turn boundary through
    /// `snapshot`, applies turn updates through `apply_update`, drains input
    /// queues through `drain`, and emits every lifecycle event through
    /// `process_event` — state reduction first, weak observers second, the
    /// strong commitment last.
    [[nodiscard]] static boost::asio::awaitable<support::ExpectedVoid> run_turns(std::shared_ptr<Impl> impl,
            std::shared_ptr<CommitmentState> commitment_state,
            std::optional<ai::UserMessage> user_message,
            InitialInput initial_input,
            std::stop_token stop_token);

    RunPolicy run_policy;
    AgentState state;
    bool active_run{false};
    std::optional<std::stop_source> active_stop_source;
    std::size_t invocation_message_offset{};
    std::uint64_t observer_diagnostic_serial_{0};
    std::size_t next_subscriber_id{1};
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    std::shared_ptr<AgentSubscriptionAnchor> subscription_anchor;
};

} // namespace cch::agent
