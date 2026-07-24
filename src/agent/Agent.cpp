#include "../../include/cch/agent/Agent.hpp"

#include "AgentLoop.hpp"
#include "../../include/cch/ai/Content.hpp"
#include "util/BoundedText.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

struct AgentSubscriptionAnchor {
    Agent::Impl* agent{nullptr};
};

std::vector<std::string> tool_names(const std::vector<ai::Tool>& definitions) {
    std::vector<std::string> names;
    names.reserve(definitions.size());
    for (const auto& definition : definitions) {
        names.push_back(definition.name);
    }
    return names;
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

    Impl(
        ai::StreamingChatClient& client,
        std::vector<ai::Tool> definitions,
        AsyncToolRegistry tools,
        AsyncAgentOptions options,
        AgentInitialState initial_state)
        : loop(client, std::move(tools), std::move(options)) {
        state.messages = std::move(initial_state.messages);
        state.thinking_level = std::move(initial_state.thinking_level);
        // Every definition came from the registry now owned by the loop.
        state.active_tool_names = tool_names(definitions);
    }

    [[nodiscard]] util::ExpectedVoid process_event(
        const AgentLifecycleEvent& event,
        const std::vector<std::shared_ptr<Subscriber>>& run_subscribers,
        AgentEventCommitter& commitment,
        std::optional<util::Error>& commitment_failure) {
        reduce_state(event);

        std::optional<AgentLifecycleEvent> invocation_event;
        const AgentLifecycleEvent* delivered_event = &event;
        if (std::holds_alternative<AgentEndEvent>(event)) {
            AgentEndEvent invocation_end;
            invocation_end.messages.assign(
                state.messages.begin() + static_cast<std::ptrdiff_t>(invocation_message_offset),
                state.messages.end());
            invocation_event.emplace(std::move(invocation_end));
            delivered_event = &*invocation_event;
        }

        // Weak observers cannot veto progress. The separately named strong
        // commitment runs only after state and every observer saw the event.
        (void)notify(*delivered_event, run_subscribers);
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
            return;
        }
        if (const auto* end = std::get_if<MessageEndEvent>(&event)) {
            state.messages.push_back(end->message);
            if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
                state.streaming_message.reset();
            }
            return;
        }
        if (const auto* start = std::get_if<ToolExecutionStartEvent>(&event)) {
            if (std::find(
                    state.pending_tool_call_ids.begin(),
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
        if (std::holds_alternative<AgentEndEvent>(event)) {
            state.streaming_message.reset();
        }
    }

    void record_observer_diagnostic(const util::Error& failure) {
        constexpr std::size_t max_diagnostics = 16;
        constexpr std::size_t max_detail_bytes = 1024;

        std::string detail = failure.message;
        if (!failure.detail.empty()) {
            detail += ": ";
            detail += failure.detail;
        }
        detail = util::bounded_redacted_text(
            std::move(detail), max_detail_bytes, "...");

        if (state.diagnostics.size() == max_diagnostics) {
            state.diagnostics.erase(state.diagnostics.begin());
        }
        state.diagnostics.push_back(util::make_error(
            failure.code,
            "agent event observer failed",
            std::move(detail)));
    }

    [[nodiscard]] util::ExpectedVoid notify(
        const AgentLifecycleEvent& event,
        const std::vector<std::shared_ptr<Subscriber>>& run_subscribers) {
        for (const auto& subscriber : run_subscribers) {
            if (!subscriber->delivery_enabled || !subscriber->sink) {
                continue;
            }
            try {
                if (auto observed = subscriber->sink(event); !observed) {
                    record_observer_diagnostic(observed.error());
                    subscriber->registered = false;
                    subscriber->delivery_enabled = false;
                }
            } catch (const std::exception& exception) {
                record_observer_diagnostic(util::make_error(
                    util::ErrorCode::Unknown,
                    exception.what()));
                subscriber->registered = false;
                subscriber->delivery_enabled = false;
            } catch (...) {
                record_observer_diagnostic(util::make_error(
                    util::ErrorCode::Unknown,
                    "unknown exception"));
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
            subscribers,
            [](const std::shared_ptr<Subscriber>& subscriber) {
                return !subscriber->registered;
            });
    }

    void unsubscribe(std::size_t id) {
        for (const auto& subscriber : subscribers) {
            if (subscriber->id == id) {
                // Run-start snapshots retain this observer through the current
                // run. Removing registration affects the next run.
                subscriber->registered = false;
                break;
            }
        }
        // Keep unregistered entries reachable while a run snapshot may still
        // own them so a reentrant close can disable later delivery.
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
        return std::ranges::any_of(
            subscribers,
            [id](const std::shared_ptr<Subscriber>& subscriber) {
                return subscriber->id == id && subscriber->registered;
            });
    }

    AsyncAgentLoop loop;
    AgentState state;
    bool active_run{false};
    std::size_t invocation_message_offset{};
    std::size_t next_subscriber_id{1};
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    std::shared_ptr<AgentSubscriptionAnchor> subscription_anchor;
};

AgentEventSubscription::AgentEventSubscription(
    AgentEventSubscription&& other) noexcept = default;

AgentEventSubscription& AgentEventSubscription::operator=(
    AgentEventSubscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

AgentEventSubscription::~AgentEventSubscription() {
    unsubscribe();
}

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

Agent::Agent(
    ai::StreamingChatClient& client,
    AsyncToolRegistry tools,
    AsyncAgentOptions options,
    AgentInitialState initial_state) {
    options.thinking_level = initial_state.thinking_level;
    auto definitions = tools.definitions();
    const auto model = options.model;
    impl_ = std::make_shared<Impl>(
        client,
        std::move(definitions),
        std::move(tools),
        std::move(options),
        std::move(initial_state));
    impl_->state.model = model;
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

boost::asio::awaitable<util::ExpectedVoid> Agent::prompt(
    std::string user_prompt) {
    co_return co_await prompt(std::move(user_prompt), {});
}

boost::asio::awaitable<util::ExpectedVoid> Agent::prompt(
    std::string user_prompt,
    AgentEventCommitter commitment) {
    auto impl = impl_;
    if (!impl) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (impl->active_run) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "agent is busy (prompt already in flight)"));
    }

    impl->active_run = true;
    impl->state.is_running = true;
    impl->state.streaming_message.reset();
    impl->state.pending_tool_call_ids.clear();
    impl->invocation_message_offset = impl->state.messages.size();
    // Subscription changes made from callbacks take effect on the next run.
    // Shared entries keep this run's move-only sinks alive without copying.
    const auto run_subscribers = impl->subscribers;

    const auto finish_run = [impl] {
        impl->state.model = impl->loop.current_model();
        impl->state.thinking_level = impl->loop.current_thinking_level();
        impl->state.streaming_message.reset();
        impl->state.pending_tool_call_ids.clear();
        impl->state.is_running = false;
        impl->active_run = false;
        impl->remove_unregistered_subscribers();
    };

    std::optional<util::Error> commitment_failure;
    std::optional<util::Expected<AsyncAgentRunResult>> result;
    try {
        result = co_await impl->loop.continue_with(
            impl->state.messages,
            std::move(user_prompt),
            [impl, run_subscribers, &commitment, &commitment_failure](
                const AgentLifecycleEvent& event) {
                return impl->process_event(
                    event, run_subscribers, commitment, commitment_failure);
            });
    } catch (const std::exception& exception) {
        finish_run();
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "agent run failed",
            exception.what()));
    } catch (...) {
        finish_run();
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "agent run failed",
            "unknown exception"));
    }

    if (*result) {
        impl->state.model = (*result)->state.model;
        if (!(*result)->state.thinking_level.empty()) {
            impl->state.thinking_level = (*result)->state.thinking_level;
        }
    }
    finish_run();

    if (commitment_failure) {
        co_return std::unexpected(std::move(*commitment_failure));
    }
    if (!*result) {
        co_return std::unexpected(result->error());
    }
    co_return util::ExpectedVoid{};
}

AgentState Agent::state() const {
    return impl_ ? impl_->state : AgentState{};
}

util::Expected<AgentEventSubscription> Agent::subscribe(
    AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (!sink) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "agent event sink is empty"));
    }

    const auto id = impl_->next_subscriber_id++;
    impl_->subscribers.push_back(std::make_shared<Impl::Subscriber>(
        Impl::Subscriber{id, std::move(sink), true, true}));

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

} // namespace cch::agent
