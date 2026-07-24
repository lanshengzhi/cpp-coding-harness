#include "../../include/cch/coding_agent/Sdk.hpp"

#include "coding_agent/runtime/AgentSessionPromptAccess.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

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

util::ExpectedVoid AgentSession::prompt(std::string text, PromptOptions options) {
    return detail::AgentSessionPromptAccess::prompt(
        *this,
        std::move(text),
        options.expand_prompt_templates,
        {});
}

util::ExpectedVoid detail::AgentSessionPromptAccess::prompt(
    AgentSession& session,
    std::string text,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    // Retain the implementation independently of the public handle so close or
    // destruction from an observer cannot invalidate the active callback stack.
    auto impl = session.impl_;
    if (!impl) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (impl->state == AgentSession::Impl::State::Closing ||
        impl->state == AgentSession::Impl::State::Closed) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is closed"));
    }
    if (impl->state == AgentSession::Impl::State::RunningPrompt) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }

    impl->state = AgentSession::Impl::State::RunningPrompt;
    ScopeExit restore_state{[impl] {
        if (impl->state == AgentSession::Impl::State::Closing) {
            impl->state = AgentSession::Impl::State::Closed;
        } else if (impl->state == AgentSession::Impl::State::RunningPrompt) {
            impl->state = AgentSession::Impl::State::Open;
        }
    }};

    return impl->runtime->run_prompt(
        std::move(text),
        expand_prompt_templates,
        std::move(on_preflight_accepted));
}

util::Expected<EventSubscription> AgentSession::subscribe(agent::AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (impl_->state == AgentSession::Impl::State::Closing ||
        impl_->state == AgentSession::Impl::State::Closed) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is closed"));
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
