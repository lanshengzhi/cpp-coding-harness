#include "coding_agent/AgentSession.hpp"

#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/AgentSessionPromptAccess.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent {
namespace {

thread_local const AgentSession::Impl* blocking_prompt_wait = nullptr;

class BlockingPromptWaitScope final {
public:
    explicit BlockingPromptWaitScope(const AgentSession::Impl* impl)
        : previous_(blocking_prompt_wait) {
        blocking_prompt_wait = impl;
    }
    BlockingPromptWaitScope(const BlockingPromptWaitScope&) = delete;
    BlockingPromptWaitScope& operator=(const BlockingPromptWaitScope&) = delete;
    ~BlockingPromptWaitScope() { blocking_prompt_wait = previous_; }

private:
    const AgentSession::Impl* previous_;
};

[[nodiscard]] util::ExpectedVoid prompt_exception(std::exception_ptr exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine failed",
            error.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine failed",
            "unknown exception"));
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Pimpl definitions
// ─────────────────────────────────────────────────────────────────────────────

struct EventSubscription::Impl {
    explicit Impl(agent::AgentEventSubscription subscription_value)
        : subscription(std::move(subscription_value)) {}

    agent::AgentEventSubscription subscription;
};

struct AgentSession::Impl {
    std::optional<std::filesystem::path> session_path;
    std::unique_ptr<runtime::AgentSessionRuntime> runtime;
};

// ─────────────────────────────────────────────────────────────────────────────
// EventSubscription implementation
// ─────────────────────────────────────────────────────────────────────────────

EventSubscription::EventSubscription(EventSubscription&& other) noexcept
    : impl_(std::move(other.impl_)) {}

EventSubscription& EventSubscription::operator=(EventSubscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

EventSubscription::~EventSubscription() {
    unsubscribe();
}

void EventSubscription::unsubscribe() {
    if (!impl_) return;
    impl_->subscription.unsubscribe();
    impl_.reset();
}

EventSubscription::operator bool() const {
    return impl_ && static_cast<bool>(impl_->subscription);
}

// ─────────────────────────────────────────────────────────────────────────────
// AgentSession implementation
// ─────────────────────────────────────────────────────────────────────────────

AgentSession::AgentSession() = default;
AgentSession::AgentSession(AgentSession&&) noexcept = default;

AgentSession& AgentSession::operator=(AgentSession&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            close();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

AgentSession::~AgentSession() {
    if (impl_) {
        close();
    }
}

boost::asio::awaitable<util::ExpectedVoid> AgentSession::prompt(
    std::string text,
    PromptOptions options) {
    // AgentSessionPromptAccess::prompt is deliberately an ordinary function
    // (no coroutine keywords): session.impl_ is copied into the prompt_impl
    // frame synchronously at the call, so moving or closing the public handle
    // before the first co_await cannot invalidate the returned lazy awaitable.
    return detail::AgentSessionPromptAccess::prompt(
        *this,
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates,
        {});
}

util::ExpectedVoid AgentSession::prompt_blocking(
    std::string text,
    PromptOptions options) {
    return detail::AgentSessionPromptAccess::prompt_blocking(
        *this,
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates,
        {});
}

util::ExpectedVoid AgentSession::steer(
    std::string text,
    PromptOptions options) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->steer(
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates);
}

util::ExpectedVoid AgentSession::follow_up(
    std::string text,
    PromptOptions options) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->follow_up(
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates);
}

util::ExpectedVoid AgentSession::set_steering_mode(agent::InputQueueMode mode) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_steering_mode(mode);
}

util::ExpectedVoid AgentSession::set_follow_up_mode(agent::InputQueueMode mode) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_follow_up_mode(mode);
}

util::ExpectedVoid AgentSession::clear_steering_queue() {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->clear_steering_queue();
}

util::ExpectedVoid AgentSession::clear_follow_up_queue() {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->clear_follow_up_queue();
}

util::ExpectedVoid AgentSession::clear_input_queues() {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->clear_input_queues();
}

util::Expected<std::string> AgentSession::set_thinking_level(
    std::string_view level) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_thinking_level(level);
}

boost::asio::awaitable<util::Expected<CompactionResult>> AgentSession::compact(
    std::string custom_instructions) {
    // Same impl_ copying contract as prompt(): the lazy awaitable is safe to
    // return even if the public handle moves or is destroyed first.
    return detail::AgentSessionPromptAccess::compact(
        *this, std::move(custom_instructions));
}

bool detail::AgentSessionInteractiveAccess::has_user_shell(
    const AgentSession& session) {
    return session.impl_ && session.impl_->runtime &&
        session.impl_->runtime->has_user_shell();
}

boost::asio::awaitable<util::Expected<runtime::UserBashCompletion>>
detail::AgentSessionInteractiveAccess::run_user_bash(
    AgentSession& session,
    std::string command,
    bool exclude_from_context,
    runtime::UserBashProgressSink progress_sink) {
    return run_user_bash_impl(
        session.impl_,
        std::move(command),
        exclude_from_context,
        std::move(progress_sink));
}

boost::asio::awaitable<util::Expected<runtime::UserBashCompletion>>
detail::AgentSessionInteractiveAccess::run_user_bash_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string command,
    bool exclude_from_context,
    runtime::UserBashProgressSink progress_sink) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    co_return co_await impl->runtime->run_user_bash(
        std::move(command),
        exclude_from_context,
        std::move(progress_sink));
}

void detail::AgentSessionInteractiveAccess::cancel_user_bash(
    AgentSession& session) {
    if (session.impl_ && session.impl_->runtime) {
        session.impl_->runtime->cancel_user_bash();
    }
}

boost::asio::awaitable<util::ExpectedVoid> detail::AgentSessionPromptAccess::prompt(
    AgentSession& session,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    return prompt_impl(
        session.impl_,
        std::move(text),
        std::move(images),
        expand_prompt_templates,
        std::move(on_preflight_accepted));
}

boost::asio::awaitable<util::ExpectedVoid> detail::AgentSessionPromptAccess::prompt_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    try {
        co_return co_await impl->runtime->run_prompt(
            std::move(text),
            std::move(images),
            expand_prompt_templates,
            std::move(on_preflight_accepted));
    } catch (...) {
        co_return prompt_exception(std::current_exception());
    }
}

boost::asio::awaitable<util::Expected<CompactionResult>>
detail::AgentSessionPromptAccess::compact(
    AgentSession& session,
    std::string custom_instructions) {
    return compact_impl(session.impl_, std::move(custom_instructions));
}

boost::asio::awaitable<util::Expected<CompactionResult>>
detail::AgentSessionPromptAccess::compact_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string custom_instructions) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    try {
        co_return co_await impl->runtime->compact(std::move(custom_instructions));
    } catch (...) {
        const auto failure = prompt_exception(std::current_exception());
        co_return std::unexpected(failure.error());
    }
}

util::ExpectedVoid detail::AgentSessionPromptAccess::prompt_blocking(
    AgentSession& session,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    const auto impl = session.impl_;
    if (!impl) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    if (blocking_prompt_wait == impl.get()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> result;
    std::exception_ptr exception;
    BlockingPromptWaitScope wait_scope{impl.get()};

    boost::asio::co_spawn(
        io,
        prompt(
            session,
            std::move(text),
            std::move(images),
            expand_prompt_templates,
            std::move(on_preflight_accepted)),
        [&](std::exception_ptr completion_exception, util::ExpectedVoid completion) {
            exception = completion_exception;
            result.emplace(std::move(completion));
        });
    io.run();

    if (exception) {
        return prompt_exception(exception);
    }
    if (!result) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine did not complete"));
    }
    return std::move(*result);
}

util::Expected<EventSubscription> AgentSession::subscribe(agent::AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (!sink) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "event sink is empty"));
    }

    auto subscribed = impl_->runtime->subscribe(std::move(sink));
    if (!subscribed) {
        return std::unexpected(subscribed.error());
    }

    auto sub_impl = std::make_unique<EventSubscription::Impl>(
        std::move(*subscribed));

    EventSubscription sub;
    sub.impl_ = std::move(sub_impl);
    return sub;
}

util::Expected<SessionEventSubscription> AgentSession::subscribe_session(
    AgentSessionEventSink sink) {
    if (!impl_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    if (!sink) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session event sink is empty"));
    }
    return impl_->runtime->subscribe_session(std::move(sink));
}

AgentSessionSnapshot AgentSession::snapshot() const {
    if (!impl_ || !impl_->runtime) {
        return {};
    }
    return impl_->runtime->snapshot(impl_->session_path);
}

std::size_t AgentSession::message_count() const {
    return impl_ && impl_->runtime ? impl_->runtime->message_count() : 0;
}

std::optional<std::string> AgentSession::last_assistant_text() const {
    return impl_ && impl_->runtime ? impl_->runtime->last_assistant_text() : std::nullopt;
}

const std::string& AgentSession::session_id() const {
    static const std::string empty;
    return impl_ && impl_->runtime ? impl_->runtime->session_id() : empty;
}

const std::optional<std::filesystem::path>& AgentSession::session_path() const {
    static const std::optional<std::filesystem::path> absent;
    return impl_ ? impl_->session_path : absent;
}

const std::string& AgentSession::provider() const {
    static const std::string empty;
    return impl_ && impl_->runtime ? impl_->runtime->provider() : empty;
}

const std::string& AgentSession::model() const {
    static const std::string empty;
    return impl_ && impl_->runtime ? impl_->runtime->model() : empty;
}

std::shared_ptr<ModelRuntime> AgentSession::model_runtime() const {
    return impl_ && impl_->runtime ? impl_->runtime->model_runtime() : nullptr;
}

const std::filesystem::path& AgentSession::workspace() const {
    static const std::filesystem::path empty;
    return impl_ && impl_->runtime ? impl_->runtime->workspace() : empty;
}

void AgentSession::abort() {
    if (impl_ && impl_->runtime) {
        impl_->runtime->abort();
    }
}

void AgentSession::close() noexcept {
    if (impl_ && impl_->runtime) {
        impl_->runtime->close();
    }
}

bool AgentSession::is_open() const {
    return impl_ && impl_->runtime && impl_->runtime->is_open();
}

bool AgentSession::is_busy() const {
    return impl_ && impl_->runtime && impl_->runtime->is_busy();
}

const std::vector<Skill>& AgentSession::skills() const {
    static const std::vector<Skill> empty;
    return impl_ && impl_->runtime ? impl_->runtime->skills() : empty;
}

const std::vector<PromptTemplate>& AgentSession::templates() const {
    static const std::vector<PromptTemplate> empty;
    return impl_ && impl_->runtime ? impl_->runtime->templates() : empty;
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

class AgentSessionRuntimeAccess {
public:
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> wrap_factory_result(
        util::Expected<runtime::CreateAgentSessionResult> factory_result) {
        if (!factory_result) {
            return std::unexpected(factory_result.error());
        }

        auto session = std::make_unique<AgentSession>();
        session->impl_ = std::make_shared<AgentSession::Impl>();
        session->impl_->session_path = factory_result->session_path;
        session->impl_->runtime = std::move(factory_result->runtime);

        CreateAgentSessionResult result;
        result.session = std::move(session);
        result.diagnostics = std::move(factory_result->diagnostics);
        result.model_fallback_message =
            std::move(factory_result->model_fallback_message);
        result.session_id = factory_result->session_id;
        result.provider = factory_result->provider;
        result.model = factory_result->model;
        result.session_path = std::move(factory_result->session_path);
        result.workspace = factory_result->workspace;
        result.metadata = factory_result->metadata;
        return result;
    }
};

} // namespace detail

util::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request) {
    return detail::AgentSessionRuntimeAccess::wrap_factory_result(
        runtime::SessionFactory::create(std::move(request)));
}

util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models) {
    return detail::AgentSessionRuntimeAccess::wrap_factory_result(
        runtime::SessionFactory::create(
            std::move(request), std::move(models)));
}

util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<runtime::AsyncUserShell> user_shell) {
    return detail::AgentSessionRuntimeAccess::wrap_factory_result(
        runtime::SessionFactory::create(
            std::move(request), std::move(models), std::move(user_shell)));
}

} // namespace cch::coding_agent
