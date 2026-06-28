#include "../../include/cch/coding_agent/Sdk.hpp"

#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent {

// ─────────────────────────────────────────────────────────────────────────────
// Pimpl definitions
// ─────────────────────────────────────────────────────────────────────────────

struct EventSubscription::Impl {
    int subscriber_index{-1};
    AgentSession::Impl* session{nullptr};
};

struct AgentSession::Impl {
    enum class State { Open, RunningPrompt, Closed };

    State state{State::Closed};
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
    if (impl_->session && impl_->session->runtime && impl_->subscriber_index >= 0) {
        impl_->session->runtime->unsubscribe(impl_->subscriber_index);
    }
    impl_.reset();
}

EventSubscription::operator bool() const {
    if (!impl_) return false;
    if (!impl_->session) return false;
    if (!impl_->session->runtime) return false;
    if (impl_->subscriber_index < 0) return false;
    return impl_->session->runtime->is_subscribed(impl_->subscriber_index);
}

// ─────────────────────────────────────────────────────────────────────────────
// AgentSession implementation
// ─────────────────────────────────────────────────────────────────────────────

AgentSession::AgentSession() = default;
AgentSession::AgentSession(AgentSession&&) noexcept = default;
AgentSession& AgentSession::operator=(AgentSession&&) noexcept = default;

AgentSession::~AgentSession() {
    if (impl_) {
        (void)close();
    }
}

util::Expected<PromptResult> AgentSession::prompt(std::string text, PromptOptions options) {
    if (!impl_) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (impl_->state == AgentSession::Impl::State::Closed) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is closed"));
    }
    if (impl_->state == AgentSession::Impl::State::RunningPrompt) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is busy (prompt already in flight)"));
    }

    impl_->state = AgentSession::Impl::State::RunningPrompt;

    auto run_result = impl_->runtime->run_prompt(std::move(text), std::move(options.event_sink));

    PromptResult result;
    result.success = run_result.success;
    result.code = run_result.code;
    result.message = run_result.message;
    result.diagnostics = std::move(run_result.diagnostics);
    result.last_assistant_text = impl_->runtime->last_assistant_text();
    result.message_count = impl_->runtime->message_count();

    impl_->state = AgentSession::Impl::State::Open;
    return result;
}

util::Expected<EventSubscription> AgentSession::subscribe(agent::AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is not initialized"));
    }
    if (impl_->state == AgentSession::Impl::State::Closed) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "session is closed"));
    }
    if (!sink) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "event sink is empty"));
    }

    int id = impl_->runtime->subscribe(std::move(sink));
    if (id < 0) {
        return std::unexpected(util::make_error(util::ErrorCode::Validation, "failed to subscribe"));
    }

    auto sub_impl = std::make_unique<EventSubscription::Impl>();
    sub_impl->subscriber_index = id;
    sub_impl->session = impl_.get();

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

const std::filesystem::path& AgentSession::session_path() const {
    static const std::filesystem::path empty;
    return impl_ && impl_->runtime ? impl_->runtime->session_path() : empty;
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
    if (!impl_) return {};
    if (impl_->state == AgentSession::Impl::State::Closed) return {};

    impl_->state = AgentSession::Impl::State::Closed;
    impl_->runtime->close();
    return {};
}

bool AgentSession::is_open() const {
    return impl_ && impl_->runtime && impl_->runtime->is_open();
}

bool AgentSession::is_busy() const {
    return impl_ && impl_->state == AgentSession::Impl::State::RunningPrompt;
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

util::Expected<CreateAgentSessionResult> create_agent_session(
    CreateAgentSessionOptions options) {
    auto factory_result = runtime::SessionFactory::create(std::move(options));
    if (!factory_result) {
        return std::unexpected(factory_result.error());
    }

    auto session = std::make_unique<AgentSession>();
    session->impl_ = std::make_unique<AgentSession::Impl>();
    session->impl_->state = AgentSession::Impl::State::Open;
    session->impl_->runtime = std::move(factory_result->runtime);

    CreateAgentSessionResult result;
    result.session = std::move(session);
    result.diagnostics = std::move(factory_result->diagnostics);
    result.session_id = factory_result->session_id;
    result.provider = factory_result->provider;
    result.model = factory_result->model;
    result.session_path = factory_result->session_path;
    result.workspace = factory_result->workspace;
    return result;
}

} // namespace cch::coding_agent
