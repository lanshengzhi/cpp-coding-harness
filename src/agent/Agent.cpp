#include <cch/agent/Agent.hpp>

#include "AgentLoop.hpp"
#include "agent/AgentMessageAccess.hpp"
#include "ai/AsyncResultBridge.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Model.hpp>
#include "ai/BoundedText.hpp"

#include <algorithm>
#include <cstddef>
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#include <exception>
#endif
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

struct AgentSubscriptionAnchor {
    Agent::Impl* agent{nullptr};
};

[[nodiscard]] support::ExpectedVoid invoke_weak_observer(
    AgentEventSink& sink,
    const AgentLifecycleEvent& event) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        return sink(event);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& exception) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            exception.what()));
    } catch (...) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "unknown exception"));
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
                    for (const auto& block :
                         std::get<std::vector<ai::Content>>(current.content)) {
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
    AgentInputQueues& queues,
    AgentInputQueue& queue,
    ai::MessageVariant message,
    std::string_view queue_name) {
    const std::size_t message_bytes = approximate_message_size(message);
    if (queue.messages.size() + 1 > queues.max_messages) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "too many queued messages",
            std::string{queue_name} + " message count exceeds " + std::to_string(queues.max_messages)));
    }

    std::size_t queued_bytes = message_bytes;
    for (const auto& queued : queue.messages) {
        queued_bytes += approximate_message_size(queued);
    }
    if (queued_bytes > queues.max_bytes) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "queued messages too large",
            std::string{queue_name} + " message byte size exceeds " + std::to_string(queues.max_bytes)));
    }

    queue.messages.push_back(std::move(message));
    return {};
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
        ai::ModelStreamFactory stream_factory,
        std::vector<ai::Tool> definitions,
        ToolRegistry tools,
        AsyncAgentOptions options,
        AgentInitialState initial_state)
        : loop(std::move(stream_factory), std::move(tools), std::move(options)) {
        state.messages = std::move(initial_state.messages);
        // The loop clamps the requested level against the active model at
        // construction; live state reflects the effective (clamped) level so
        // `state()` and the wire never diverge (#352).
        state.thinking_level = loop.current_thinking_level();
        // The session System Prompt lives on the loop options (pi
        // `AgentState.systemPrompt`) and is mirrored into live state like the
        // model and thinking level.
        state.system_prompt = loop.current_system_prompt();
        state.input_queues.max_messages = loop.max_queued_messages();
        state.input_queues.max_bytes = loop.max_queued_bytes();
        state.input_queues.steering.mode = loop.steering_mode();
        state.input_queues.follow_up.mode = loop.follow_up_mode();
        // Every definition came from the registry now owned by the loop.
        state.active_tool_names = tool_names(definitions);
    }

    [[nodiscard]] support::ExpectedVoid process_event(
        const AgentLifecycleEvent& event,
        AgentEventCommitter& commitment,
        std::optional<support::Error>& commitment_failure) {
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

    void record_observer_diagnostic(const support::Error& failure) {
        constexpr std::size_t kMaxDiagnostics = 16;
        constexpr std::size_t kMaxDetailBytes = 1024;

        std::string detail = failure.message;
        if (!failure.detail.empty()) {
            detail += ": ";
            detail += failure.detail;
        }
        detail = ai::bounded_redacted_text(
            std::move(detail), kMaxDetailBytes, "...");

        if (state.diagnostics.size() == kMaxDiagnostics) {
            state.diagnostics.erase(state.diagnostics.begin());
        }
        state.diagnostics.push_back(support::make_error(
            failure.code,
            "agent event observer failed",
            std::move(detail)));
    }

    [[nodiscard]] support::ExpectedVoid notify(
        const AgentLifecycleEvent& event,
        const std::vector<std::shared_ptr<Subscriber>>& delivery_snapshot) {
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
            subscribers,
            [](const std::shared_ptr<Subscriber>& subscriber) {
                return !subscriber->registered;
            });
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
        return std::ranges::any_of(
            subscribers,
            [id](const std::shared_ptr<Subscriber>& subscriber) {
                return subscriber->id == id && subscriber->registered;
            });
    }

    /// Shared execution body for `prompt` and `continue_run`: installs the
    /// run-stop source, drives the loop through `run_loop_call` with the
    /// run-scoped event sink and stop token, and settles run state on every
    /// exit path. `run_loop_call` is built by the caller (a friend context)
    /// because the loop's private input-queue kinds are not reachable here.
    [[nodiscard]] static boost::asio::awaitable<support::ExpectedVoid> run_loop(
        std::shared_ptr<Impl> impl,
        std::move_only_function<boost::asio::awaitable<support::Expected<AsyncAgentRunResult>>(
            AgentEventSink sink, std::stop_token token)> run_loop_call,
        AgentEventCommitter commitment,
        std::stop_source stop_source) {
        impl->active_run = true;
        impl->active_stop_source.emplace(std::move(stop_source));
        impl->state.is_running = true;
        impl->state.streaming_message.reset();
        impl->state.pending_tool_call_ids.clear();
        impl->invocation_message_offset = impl->state.messages.size();

        const auto finish_run = [impl] {
            impl->state.model = impl->loop.current_model();
            impl->state.thinking_level = impl->loop.current_thinking_level();
            impl->state.system_prompt = impl->loop.current_system_prompt();
            impl->state.streaming_message.reset();
            impl->state.pending_tool_call_ids.clear();
            impl->state.is_running = false;
            impl->active_stop_source.reset();
            impl->active_run = false;
            impl->remove_unregistered_subscribers();
        };

        std::optional<support::Error> commitment_failure;
        auto result = co_await run_loop_call(
            [impl, &commitment, &commitment_failure](
                const AgentLifecycleEvent& event) {
                return impl->process_event(
                    event, commitment, commitment_failure);
            },
            impl->active_stop_source->get_token());

        if (result) {
            impl->state.model = result->state.model;
            if (!result->state.thinking_level.empty()) {
                impl->state.thinking_level = result->state.thinking_level;
            }
        }
        finish_run();

        if (commitment_failure) {
            co_return std::unexpected(std::move(*commitment_failure));
        }
        if (!result) {
            co_return std::unexpected(result.error());
        }
        co_return support::ExpectedVoid{};
    }

    AsyncAgentLoop loop;
    AgentState state;
    bool active_run{false};
    std::optional<std::stop_source> active_stop_source;
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
    ai::ModelStreamFactory stream_factory,
    ToolRegistry tools,
    AsyncAgentOptions options,
    AgentInitialState initial_state) {
    options.thinking_level = initial_state.thinking_level;
    auto definitions = tools.definitions();
    impl_ = std::make_shared<Impl>(
        std::move(stream_factory),
        std::move(definitions),
        std::move(tools),
        std::move(options),
        std::move(initial_state));
    impl_->state.model = impl_->loop.current_model();
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

support::AsyncResult<void> Agent::prompt(std::string user_prompt) {
    return prompt(std::move(user_prompt), {});
}

support::AsyncResult<void> Agent::prompt(
    std::string user_prompt,
    AgentEventCommitter commitment) {
    return prompt(
        ai::user_text_message(std::move(user_prompt)),
        std::move(commitment),
        std::stop_source{});
}

support::AsyncResult<void> Agent::prompt(
    ai::UserMessage user_message,
    AgentEventCommitter commitment,
    std::stop_source stop_source) {
    if (!impl_) {
        return support::AsyncResult<void>{std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"))};
    }
    if (impl_->active_run) {
        return support::AsyncResult<void>{std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is busy (prompt already in flight)"))};
    }
    return ai::detail::make_async_result(
        [impl = impl_,
         user_message = std::move(user_message),
         commitment = std::move(commitment),
         stop_source = std::move(stop_source)]() mutable
            -> boost::asio::awaitable<support::ExpectedVoid> {
    if (!impl) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (impl->active_run) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is busy (prompt already in flight)"));
    }
    co_return co_await Impl::run_loop(
        impl,
        [impl, user_message = std::move(user_message)](
            AgentEventSink sink,
            std::stop_token token) mutable {
            return impl->loop.continue_with(
                impl->state.messages,
                std::move(user_message),
                std::move(sink),
                token,
                [impl](AsyncAgentLoop::InputQueueKind queue_kind) {
                    auto& queue =
                        queue_kind == AsyncAgentLoop::InputQueueKind::Steering
                            ? impl->state.input_queues.steering.messages
                            : impl->state.input_queues.follow_up.messages;
                    const auto mode =
                        queue_kind == AsyncAgentLoop::InputQueueKind::Steering
                            ? impl->state.input_queues.steering.mode
                            : impl->state.input_queues.follow_up.mode;
                    if (queue.empty()) {
                        return std::vector<ai::MessageVariant>{};
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
                });
        },
        std::move(commitment),
        std::move(stop_source));
        });
}

support::AsyncResult<void> Agent::continue_run(
    AgentEventCommitter commitment,
    std::stop_source stop_source) {
    return ai::detail::make_async_result(
        [impl = impl_,
         commitment = std::move(commitment),
         stop_source = std::move(stop_source)]() mutable
            -> boost::asio::awaitable<support::ExpectedVoid> {
    if (!impl) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (impl->active_run) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is busy (prompt already in flight)"));
    }
    co_return co_await Impl::run_loop(
        impl,
        [impl](
            AgentEventSink sink,
            std::stop_token token) {
            return impl->loop.continue_with(
                impl->state.messages,
                std::nullopt,
                std::move(sink),
                token,
                [impl](AsyncAgentLoop::InputQueueKind queue_kind) {
                    auto& queue =
                        queue_kind == AsyncAgentLoop::InputQueueKind::Steering
                            ? impl->state.input_queues.steering.messages
                            : impl->state.input_queues.follow_up.messages;
                    const auto mode =
                        queue_kind == AsyncAgentLoop::InputQueueKind::Steering
                            ? impl->state.input_queues.steering.mode
                            : impl->state.input_queues.follow_up.mode;
                    if (queue.empty()) {
                        return std::vector<ai::MessageVariant>{};
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
                });
        },
        std::move(commitment),
        std::move(stop_source));
        });
}

void Agent::abort() {
    if (impl_ && impl_->active_stop_source) {
        (void)impl_->active_stop_source->request_stop();
    }
}

support::ExpectedVoid Agent::steer(ai::MessageVariant message) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    auto& queues = impl_->state.input_queues;
    return admit_queued_message(
        queues, queues.steering, std::move(message), "steering");
}

support::ExpectedVoid Agent::follow_up(ai::MessageVariant message) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    auto& queues = impl_->state.input_queues;
    return admit_queued_message(
        queues, queues.follow_up, std::move(message), "follow-up");
}

support::ExpectedVoid Agent::set_steering_mode(InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    impl_->state.input_queues.steering.mode = mode;
    return {};
}

support::ExpectedVoid Agent::set_follow_up_mode(InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    impl_->state.input_queues.follow_up.mode = mode;
    return {};
}

namespace {

[[nodiscard]] bool is_valid_thinking_level(std::string_view level) {
    static const std::vector<std::string> allowed{
        "off", "minimal", "low", "medium", "high", "xhigh", "max"};
    return std::find(allowed.begin(), allowed.end(), level) != allowed.end();
}

} // namespace

support::Expected<std::string> Agent::set_thinking_level(std::string_view level) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (!is_valid_thinking_level(level)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "invalid thinking level",
            std::string{level}));
    }
    // Clamp against the active model so an unsupported level can never be
    // forwarded to the stream (pi agent-session.ts setThinkingLevel clamps
    // before persisting; a clamped level equal to the current one is a no-op).
    const auto effective = ai::clamp_thinking_level_string(
        impl_->loop.current_model(), level);
    impl_->loop.set_thinking_level(effective);
    impl_->state.thinking_level = effective;
    return effective;
}

support::ExpectedVoid Agent::set_model(ai::Model model) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (auto valid = ai::validate_model(model); !valid) {
        return valid;
    }
    impl_->loop.set_model(model);
    impl_->state.model = std::move(model);
    return {};
}

void Agent::set_system_prompt(std::string system_prompt) {
    if (!impl_) {
        return;
    }
    impl_->loop.set_system_prompt(system_prompt);
    impl_->state.system_prompt = std::move(system_prompt);
}

support::ExpectedVoid Agent::clear_steering_queue() {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    impl_->state.input_queues.steering.messages.clear();
    return {};
}

support::ExpectedVoid Agent::clear_follow_up_queue() {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
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

AgentState Agent::state() const {
    return impl_ ? impl_->state : AgentState{};
}

support::Expected<AgentEventSubscription> Agent::subscribe(
    AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (!sink) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

support::ExpectedVoid detail::AgentMessageAccess::append_bash_execution(
    Agent& agent,
    ai::BashExecutionMessage message) {
    if (!agent.impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (agent.impl_->active_run) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is busy (cannot commit passive message)"));
    }
    agent.impl_->state.messages.emplace_back(std::move(message));
    return {};
}

support::ExpectedVoid detail::AgentMessageAccess::replace_messages(
    Agent& agent,
    std::vector<ai::MessageVariant> messages) {
    if (!agent.impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (agent.impl_->active_run) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is busy (cannot replace session context)"));
    }
    agent.impl_->state.messages = std::move(messages);
    agent.impl_->state.streaming_message.reset();
    agent.impl_->state.pending_tool_call_ids.clear();
    return {};
}

support::ExpectedVoid detail::AgentMessageAccess::pop_trailing_assistant(
    Agent& agent) {
    if (!agent.impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is not initialized"));
    }
    if (agent.impl_->active_run) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "agent is busy (cannot mutate session context)"));
    }
    auto& messages = agent.impl_->state.messages;
    if (!messages.empty() &&
        std::holds_alternative<ai::AssistantMessage>(messages.back())) {
        messages.pop_back();
    }
    return {};
}

} // namespace cch::agent
