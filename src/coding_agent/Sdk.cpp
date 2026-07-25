#include "../../include/cch/coding_agent/Sdk.hpp"

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

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) : callback_(std::move(callback)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { callback_(); }

private:
    Callback callback_;
};

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
    enum class State { Open, RunningPrompt, Closing, Closed };

    /// Shared preflight outcome for entry points that require a non-closed session.
    [[nodiscard]] util::ExpectedVoid reject_if_closed() const {
        if (state == State::Closing || state == State::Closed) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "session is closed"));
        }
        return {};
    }

    /// Shared preflight outcome for entry points that reject a concurrent prompt.
    [[nodiscard]] util::ExpectedVoid reject_if_busy() const {
        if (state == State::RunningPrompt) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "session is busy (prompt already in flight)"));
        }
        return {};
    }

    State state{State::Closed};
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
            (void)close();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

AgentSession::~AgentSession() {
    if (impl_) {
        (void)close();
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
        options.expand_prompt_templates,
        {});
}

util::ExpectedVoid AgentSession::prompt_blocking(
    std::string text,
    PromptOptions options) {
    return detail::AgentSessionPromptAccess::prompt_blocking(
        *this,
        std::move(text),
        options.expand_prompt_templates,
        {});
}

boost::asio::awaitable<util::ExpectedVoid> detail::AgentSessionPromptAccess::prompt(
    AgentSession& session,
    std::string text,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    return prompt_impl(
        session.impl_,
        std::move(text),
        expand_prompt_templates,
        std::move(on_preflight_accepted));
}

boost::asio::awaitable<util::ExpectedVoid> detail::AgentSessionPromptAccess::prompt_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string text,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is not initialized"));
    }
    if (auto rejected = impl->reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = impl->reject_if_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }

    impl->state = AgentSession::Impl::State::RunningPrompt;
    ScopeExit restore_state{[impl] {
        if (impl->state == AgentSession::Impl::State::Closing) {
            impl->state = AgentSession::Impl::State::Closed;
        } else if (impl->state == AgentSession::Impl::State::RunningPrompt) {
            impl->state = AgentSession::Impl::State::Open;
        }
    }};

    try {
        co_return co_await impl->runtime->run_prompt(
            std::move(text),
            expand_prompt_templates,
            std::move(on_preflight_accepted));
    } catch (...) {
        co_return prompt_exception(std::current_exception());
    }
}

util::ExpectedVoid detail::AgentSessionPromptAccess::prompt_blocking(
    AgentSession& session,
    std::string text,
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
    if (auto rejected = impl->reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (auto rejected = impl->reject_if_busy(); !rejected) {
        return std::unexpected(rejected.error());
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
    if (auto rejected = impl_->reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
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

const std::filesystem::path& AgentSession::workspace() const {
    static const std::filesystem::path empty;
    return impl_ && impl_->runtime ? impl_->runtime->workspace() : empty;
}

util::ExpectedVoid AgentSession::close() {
    auto impl = impl_;
    if (!impl) return {};
    if (impl->state == AgentSession::Impl::State::Closing ||
        impl->state == AgentSession::Impl::State::Closed) {
        return {};
    }

    if (impl->state == AgentSession::Impl::State::RunningPrompt) {
        impl->state = AgentSession::Impl::State::Closing;
    } else {
        impl->state = AgentSession::Impl::State::Closed;
    }
    impl->runtime->close();
    return {};
}

bool AgentSession::is_open() const {
    return impl_ && impl_->runtime && impl_->runtime->is_open();
}

bool AgentSession::is_busy() const {
    return impl_ &&
           (impl_->state == AgentSession::Impl::State::RunningPrompt ||
            impl_->state == AgentSession::Impl::State::Closing);
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
        session->impl_->state = AgentSession::Impl::State::Open;
        session->impl_->session_path = factory_result->session_path;
        session->impl_->runtime = std::move(factory_result->runtime);

        CreateAgentSessionResult result;
        result.session = std::move(session);
        result.diagnostics = std::move(factory_result->diagnostics);
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
    CreateAgentSessionOptions options) {
    return detail::AgentSessionRuntimeAccess::wrap_factory_result(runtime::SessionFactory::create(std::move(options)));
}

util::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request) {
    return detail::AgentSessionRuntimeAccess::wrap_factory_result(runtime::SessionFactory::create(std::move(request)));
}

} // namespace cch::coding_agent
