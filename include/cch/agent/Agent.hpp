#pragma once

#include "AgentContext.hpp"
#include "AgentEvent.hpp"
#include "ToolRegistry.hpp"
#include "../ai/ChatClient.hpp"
#include "../util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>
#include <vector>

namespace cch::agent {

/// Passive initial conversation state for a stateful Agent.
/// Runtime-owned fields always begin idle; model and tool state come from the
/// Agent's run options and owned tool registry.
struct AgentInitialState {
    std::vector<ai::MessageVariant> messages;
    std::string thinking_level{"off"};
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
/// The Agent and referenced chat client must outlive every prompt coroutine.
/// Agent operations and state snapshots are executor-confined; concurrent calls
/// from unrelated threads are not supported.
class Agent {
public:
    Agent(
        ai::StreamingChatClient& client,
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
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::agent
