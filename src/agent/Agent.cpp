#include <cch/agent/Agent.hpp>

#include "agent/AgentImpl.hpp"
#include "agent/AgentMessageAccess.hpp"
#include "support/AsyncResultBridge.hpp"

#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {

Agent::Impl::Impl(ai::ModelStreamFactory stream_factory,
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

[[nodiscard]] AgentExecutionSnapshot Agent::Impl::snapshot() const {
    return AgentExecutionSnapshot{
            .model = state.model,
            .thinking_level = state.thinking_level,
            .system_prompt = state.system_prompt,
            .messages = state.messages,
            .tools = run_policy.registry.definitions(),
    };
}

[[nodiscard]] support::Expected<AgentExecutionSnapshot> Agent::Impl::apply_update(AgentLoopTurnUpdate update) {
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

[[nodiscard]] std::vector<ai::MessageVariant> Agent::Impl::drain(InputQueueKind queue_kind) {
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
        return support::AsyncResult<void>{std::unexpected(agent_not_initialized())};
    }
    if (impl_->active_run) {
        return support::AsyncResult<void>{std::unexpected(agent_busy_error())};
    }
    return support::detail::make_async_result(
            [impl = impl_,
                    user_message = std::move(user_message),
                    commitment = std::move(commitment),
                    stop_source = std::move(stop_source)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                if (!impl) {
                    co_return std::unexpected(agent_not_initialized());
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
                    co_return std::unexpected(agent_not_initialized());
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
        return std::unexpected(agent_not_initialized());
    }
    auto& queues = impl_->state.input_queues;
    return admit_queued_message(queues, queues.steering, std::move(message), "steering");
}

support::ExpectedVoid Agent::follow_up(ai::MessageVariant message) {
    if (!impl_) {
        return std::unexpected(agent_not_initialized());
    }
    auto& queues = impl_->state.input_queues;
    return admit_queued_message(queues, queues.follow_up, std::move(message), "follow-up");
}

support::ExpectedVoid Agent::set_steering_mode(InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(agent_not_initialized());
    }
    impl_->state.input_queues.steering.mode = mode;
    return {};
}

support::ExpectedVoid Agent::set_follow_up_mode(InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(agent_not_initialized());
    }
    impl_->state.input_queues.follow_up.mode = mode;
    return {};
}

support::Expected<std::string> Agent::set_thinking_level(std::string_view level) {
    if (!impl_) {
        return std::unexpected(agent_not_initialized());
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
        return std::unexpected(agent_not_initialized());
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
        return std::unexpected(agent_not_initialized());
    }
    impl_->state.input_queues.steering.messages.clear();
    return {};
}

support::ExpectedVoid Agent::clear_follow_up_queue() {
    if (!impl_) {
        return std::unexpected(agent_not_initialized());
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

AgentInputQueueCounts Agent::input_queue_counts() const {
    if (!impl_) {
        return {};
    }
    return AgentInputQueueCounts{
            .steering = impl_->state.input_queues.steering.messages.size(),
            .follow_up = impl_->state.input_queues.follow_up.messages.size(),
    };
}

AgentInputQueues Agent::input_queues() const { return impl_ ? impl_->state.input_queues : AgentInputQueues{}; }

std::uint64_t Agent::observer_diagnostic_serial() const { return impl_ ? impl_->observer_diagnostic_serial_ : 0; }

std::vector<support::Error> Agent::observer_diagnostics() const {
    return impl_ ? impl_->state.diagnostics : std::vector<support::Error>{};
}

support::Expected<AgentEventSubscription> Agent::subscribe(AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(agent_not_initialized());
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
        return std::unexpected(agent_not_initialized());
    }
    if (agent.impl_->active_run) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "agent is busy (cannot commit passive message)"));
    }
    agent.impl_->state.messages.emplace_back(std::move(message));
    return {};
}

support::ExpectedVoid detail::AgentMessageAccess::replace_messages(
        Agent& agent, std::vector<ai::MessageVariant> messages) {
    if (!agent.impl_) {
        return std::unexpected(agent_not_initialized());
    }
    agent.impl_->state.messages = std::move(messages);
    agent.impl_->state.streaming_message.reset();
    agent.impl_->state.pending_tool_call_ids.clear();
    // `invocation_messages()` is a state.messages window. A between-turn
    // rebuild (pi `_runAutoCompaction` while the run continues) restarts it
    // at the replacement so the window can never address a position outside
    // the replaced vector; an idle rebuild is followed by a run start, which
    // sets the same offset itself.
    agent.impl_->invocation_message_offset = agent.impl_->state.messages.size();
    return {};
}

support::ExpectedVoid detail::AgentMessageAccess::pop_trailing_assistant(Agent& agent) {
    if (!agent.impl_) {
        return std::unexpected(agent_not_initialized());
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

