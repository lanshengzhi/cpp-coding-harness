#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::agent {
namespace detail {
class AgentMessageAccess;
class AgentPromptAccess;
} // namespace detail

/// Passive initial conversation state for a stateful Agent.
/// Runtime-owned fields always begin idle; model and tool state come from the
/// Agent's run options and owned tool registry.
struct AgentInitialState {
    std::vector<ai::MessageVariant> messages;
    /// Requested thinking level before model-aware clamping, defaulting to pi's
    /// `DEFAULT_THINKING_LEVEL` ("medium", ADR 0034 / #352). The Agent clamps
    /// this against the active model's supported set at creation, so the
    /// effective level can differ (e.g. "off" while the Agent still holds
    /// `kDefaultModel`, which supports no reasoning).
    std::string thinking_level{"medium"};
};

/// RAII handle for one weak Agent lifecycle observer.
/// Destroying or explicitly unsubscribing the handle stops event delivery.
class AgentEventSubscription {
public:
    AgentEventSubscription() = default;
    AgentEventSubscription(AgentEventSubscription&&) noexcept;
    AgentEventSubscription& operator=(AgentEventSubscription&&) noexcept;
    ~AgentEventSubscription();
    AgentEventSubscription(const AgentEventSubscription&) = delete;
    AgentEventSubscription& operator=(const AgentEventSubscription&) = delete;

    void unsubscribe();
    [[nodiscard]] explicit operator bool() const;

    struct Impl;

private:
    friend class Agent;
    std::unique_ptr<Impl> impl_;
};

/// Stateful owner of live Agent Message history and one active model run.
///
/// The Agent issues every turn through the injected `ModelRuntime::streamSimple`
/// surface (the sole injectable seam per #326, held as `std::shared_ptr`; the
/// Agent keeps the runtime alive). Agent operations and state snapshots are
/// executor-confined; concurrent calls from unrelated threads are not
/// supported.
class Agent {
public:
    Agent(
        std::shared_ptr<coding_agent::ModelRuntime> runtime,
        AsyncToolRegistry tools,
        AsyncAgentOptions options = {},
        AgentInitialState initial_state = {});
    Agent(Agent&&) noexcept;
    Agent& operator=(Agent&&) noexcept;
    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    /// Execute one prompt from the Agent's retained history.
    /// A second prompt is rejected without mutation while a run is active.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> prompt(
        std::string user_prompt);

    /// Execute one prompt with a named strong event commitment capability.
    /// For each event, Agent state advances first, weak observers run second,
    /// and the commitment runs last. A commitment failure stops the run without
    /// rolling back live state and is returned unwrapped to the caller.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> prompt(
        std::string user_prompt,
        AgentEventCommitter commitment);

    /// Request cancellation of the active run. Idempotent and a no-op while
    /// idle. The provider completes an accepted request through the ordinary
    /// assistant `aborted` lifecycle; this method adds no result channel.
    void abort();

    /// Admit an Agent Message for injection after the current assistant turn.
    /// The configured queue capacity is checked before the queue is mutated.
    [[nodiscard]] util::ExpectedVoid steer(ai::MessageVariant message);

    /// Admit an Agent Message to run when the Agent would otherwise stop.
    /// The configured queue capacity is checked before the queue is mutated.
    [[nodiscard]] util::ExpectedVoid follow_up(ai::MessageVariant message);

    /// Change the drain policy for queued steering messages.
    [[nodiscard]] util::ExpectedVoid set_steering_mode(InputQueueMode mode);

    /// Change the drain policy for queued follow-up messages.
    [[nodiscard]] util::ExpectedVoid set_follow_up_mode(InputQueueMode mode);

    /// Set the thinking level for subsequent turns (pi `setThinkingLevel`).
    /// The request is validated against the seven-level set and clamped to the
    /// active model's supported levels (`getSupportedThinkingLevels`/
    /// `clampThinkingLevel`, ADR 0034 / #352), so an unsupported level can
    /// never be forwarded to the stream; the effective (clamped) level becomes
    /// the live state. Returns the effective level, or an error for an invalid
    /// request. A request whose clamped level equals the current level is a
    /// no-op success.
    [[nodiscard]] util::Expected<std::string> set_thinking_level(
        std::string_view level);

    /// Swap the active Model for subsequent turns (pi's runtime `setModel`:
    /// `agent.state.model = model`). The Model is validated; the
    /// thinking level is untouched here — the caller re-clamps it against the
    /// new model's supported set right after, exactly like pi's `setModel` →
    /// `setThinkingLevel` sequence.
    [[nodiscard]] util::ExpectedVoid set_model(ai::Model model);

    /// Remove all pending steering messages.
    [[nodiscard]] util::ExpectedVoid clear_steering_queue();

    /// Remove all pending follow-up messages.
    [[nodiscard]] util::ExpectedVoid clear_follow_up_queue();

    /// Remove all pending steering and follow-up messages.
    [[nodiscard]] util::ExpectedVoid clear_input_queues();

    /// Return an independent passive snapshot of current live Agent state.
    [[nodiscard]] AgentState state() const;

    /// Subscribe a move-only weak observer. State is reduced before delivery.
    /// Observer failures and exceptions deactivate that observer without
    /// vetoing Agent progress.
    [[nodiscard]] util::Expected<AgentEventSubscription> subscribe(
        AgentEventSink sink);

    /// Deactivate all weak observers. Idempotent. A run-start observer
    /// snapshot remains alive for callback-stack safety but receives no later
    /// events after this call.
    void clear_subscriptions();

    struct Impl;

private:
    friend class detail::AgentMessageAccess;
    friend class detail::AgentPromptAccess;

    /// Execute one prompt using a caller-created prompt-scoped cancellation
    /// source. Copies of the source share one stop state, allowing an admission
    /// owner to request cancellation before the Agent coroutine starts.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> prompt(
        ai::UserMessage user_message,
        AgentEventCommitter commitment,
        std::stop_source stop_source);

    /// Continue the loop without a new user message (pi `agent.continue()` /
    /// `runAgentLoopContinue`), used by the session assembly's overflow
    /// compact-and-retry-once. The live message list's last message must not
    /// be an assistant message (the session removes the failed error message
    /// before continuing); the loop rejects the empty and assistant-terminal
    /// cases with pi's continuation errors.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> continue_run(
        AgentEventCommitter commitment,
        std::stop_source stop_source);

    std::shared_ptr<Impl> impl_;
};

} // namespace cch::agent
