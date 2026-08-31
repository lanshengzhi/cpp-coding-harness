#include "coding_agent/AgentSession.hpp"

#include "coding_agent/AgentSessionImpl.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/io_context.hpp>

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Pimpl definitions
// ─────────────────────────────────────────────────────────────────────────────

struct EventSubscription::Impl {
    explicit Impl(agent::AgentEventSubscription subscription_value)
        : subscription(std::move(subscription_value)) {}

    agent::AgentEventSubscription subscription;
};

struct SessionEventSubscription::Impl {
    std::size_t id{0};
    std::weak_ptr<AgentSession::Impl::SessionSubscriptionAnchor> anchor;
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
// SessionEventSubscription implementation
// ─────────────────────────────────────────────────────────────────────────────

SessionEventSubscription::SessionEventSubscription(SessionEventSubscription&&) noexcept = default;
SessionEventSubscription& SessionEventSubscription::operator=(SessionEventSubscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
SessionEventSubscription::~SessionEventSubscription() { unsubscribe(); }

void SessionEventSubscription::unsubscribe() {
    if (!impl_) {
        return;
    }
    const auto anchor = impl_->anchor.lock();
    const auto id = impl_->id;
    impl_.reset();
    if (anchor && anchor->impl) {
        auto& observers = anchor->impl->session_event_observers_;
        // Mark-only so an unsubscribe from inside an observer callback cannot
        // invalidate the delivery loop; `emit_session_event` erases
        // unregistered observers after delivery (the Agent's
        // `remove_unregistered_subscribers` pattern).
        for (const auto& observer : observers) {
            if (observer->id == id) {
                observer->registered = false;
                observer->delivery_enabled = false;
                break;
            }
        }
    }
}

SessionEventSubscription::operator bool() const {
    if (!impl_) {
        return false;
    }
    const auto anchor = impl_->anchor.lock();
    if (!anchor || !anchor->impl) {
        return false;
    }
    for (const auto& observer : anchor->impl->session_event_observers_) {
        if (observer->id == impl_->id && observer->registered) {
            return true;
        }
    }
    return false;
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

boost::asio::awaitable<support::ExpectedVoid> AgentSession::prompt(
    std::string text,
    PromptOptions options) {
    // detail::session_prompt is the lazy coroutine; the impl_ copy enters its
    // frame synchronously at this call, so moving or destroying the public
    // handle before the first co_await cannot invalidate the returned lazy
    // awaitable.
    return detail::session_prompt(impl_, std::move(text), std::move(options.images), options.expand_prompt_templates);
}

support::ExpectedVoid AgentSession::prompt_blocking(
    std::string text,
    PromptOptions options) {
    const auto impl = impl_;
    if (!impl) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
    if (blocking_prompt_wait == impl.get()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "session is busy (prompt already in flight)"));
    }
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    BlockingPromptWaitScope wait_scope{impl.get()};

    // Run the lazy coroutine on the temporary executor through the private
    // completion bridge: the bridge owns the Asio `co_spawn` completion and
    // maps its staged-build exception pointer to the operation's typed
    // outcome, so no exception-shaped code survives outside the bridge.
    auto bridged = support::detail::make_async_result_on(io.get_executor(),
            [impl,
                    text = std::move(text),
                    images = std::move(options.images),
                    expand_prompt_templates = options.expand_prompt_templates]() mutable
                    -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await detail::session_prompt(
                        impl, std::move(text), std::move(images), expand_prompt_templates);
            });
    std::move(bridged).start(
            [&result](support::ExpectedVoid completion) noexcept { result.emplace(std::move(completion)); });
    io.run();

    if (!result) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session prompt coroutine did not complete"));
    }
    return std::move(*result);
}

support::ExpectedVoid AgentSession::steer(
    std::string text,
    PromptOptions options) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->steer(std::move(text), std::move(options.images), options.expand_prompt_templates);
}

support::ExpectedVoid AgentSession::follow_up(
    std::string text,
    PromptOptions options) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->follow_up(std::move(text), std::move(options.images), options.expand_prompt_templates);
}

support::ExpectedVoid AgentSession::set_steering_mode(agent::InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->set_steering_mode(mode);
}

support::ExpectedVoid AgentSession::set_follow_up_mode(agent::InputQueueMode mode) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->set_follow_up_mode(mode);
}

support::ExpectedVoid AgentSession::clear_steering_queue() {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->clear_steering_queue();
}

support::ExpectedVoid AgentSession::clear_follow_up_queue() {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->clear_follow_up_queue();
}

support::ExpectedVoid AgentSession::clear_input_queues() {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->clear_input_queues();
}

support::Expected<std::string> AgentSession::set_thinking_level(
    std::string_view level) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->set_thinking_level(level);
}

boost::asio::awaitable<support::Expected<CompactionResult>> AgentSession::compact(
    std::string custom_instructions) {
    // Same impl_ copying contract as prompt(): the lazy awaitable is safe to
    // return even if the public handle moves or is destroyed first.
    return detail::session_compact(impl_, std::move(custom_instructions));
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::set_model(
    ai::Model model) {
    return detail::session_set_model(impl_, std::move(model));
}

support::ExpectedVoid AgentSession::set_model_blocking(ai::Model model) {
    const auto impl = impl_;
    if (!impl) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    // Same private completion bridge as prompt_blocking: the temporary
    // executor drives the lazy coroutine and the bridge owns the Asio
    // `co_spawn` completion (ADR 0042).
    auto bridged = support::detail::make_async_result_on(io.get_executor(),
            [impl, model = std::move(model)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await detail::session_set_model(impl, std::move(model));
            });
    std::move(bridged).start([&result](support::ExpectedVoid completion) noexcept {
        result.emplace(std::move(completion));
    });
    io.run();

    if (!result) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session set_model coroutine did not complete"));
    }
    return std::move(*result);
}

boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>>
AgentSession::cycle_model(std::string direction) {
    return detail::session_cycle_model(impl_, std::move(direction));
}

support::Expected<std::optional<ModelCycleResult>> AgentSession::cycle_model_blocking(
    std::string direction) {
    const auto impl = impl_;
    if (!impl) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    boost::asio::io_context io;
    std::optional<support::Expected<std::optional<ModelCycleResult>>> result;
    // Same private completion bridge as set_model_blocking: the temporary
    // executor drives the lazy coroutine and the bridge owns the Asio
    // `co_spawn` completion (ADR 0042).
    auto bridged = support::detail::make_async_result_on(io.get_executor(),
            [impl, direction = std::move(direction)]() mutable
                    -> boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>> {
                co_return co_await detail::session_cycle_model(impl, std::move(direction));
            });
    std::move(bridged).start(
        [&result](support::Expected<std::optional<ModelCycleResult>> completion) noexcept {
            result.emplace(std::move(completion));
        });
    io.run();

    if (!result) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session cycle_model coroutine did not complete"));
    }
    return std::move(*result);
}

support::Expected<std::optional<std::string>> AgentSession::cycle_thinking_level() {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->cycle_thinking_level();
}

void AgentSession::set_scoped_models(std::vector<ScopedModel> models) {
    if (impl_) {
        impl_->set_scoped_models(std::move(models));
    }
}

const std::vector<ScopedModel>& AgentSession::scoped_models() const {
    static const std::vector<ScopedModel> kEmpty;
    return impl_ ? impl_->scoped_models() : kEmpty;
}

support::Expected<EventSubscription> AgentSession::subscribe(agent::AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
    if (!sink) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "event sink is empty"));
    }
    if (auto rejected = impl_->reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!impl_->agent_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
    }

    auto subscribed = impl_->agent_->subscribe(std::move(sink));
    if (!subscribed) {
        return std::unexpected(subscribed.error());
    }

    auto sub_impl = std::make_unique<EventSubscription::Impl>(
        std::move(*subscribed));

    EventSubscription sub;
    sub.impl_ = std::move(sub_impl);
    return sub;
}

support::Expected<SessionEventSubscription> AgentSession::subscribe_session(
    AgentSessionEventSink sink) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    if (!sink) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session event sink is empty"));
    }
    if (auto rejected = impl_->reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    auto subscriber = std::make_shared<AgentSession::Impl::SessionSubscriber>(AgentSession::Impl::SessionSubscriber{
            .id = impl_->next_session_subscriber_id_++,
            .sink = std::move(sink),
    });
    const auto id = subscriber->id;
    impl_->session_event_observers_.push_back(std::move(subscriber));

    SessionEventSubscription subscription;
    subscription.impl_ = std::make_unique<SessionEventSubscription::Impl>(SessionEventSubscription::Impl{
            .id = id,
            .anchor = impl_->session_event_anchor_,
    });
    return subscription;
}

AgentSessionSnapshot AgentSession::snapshot() const {
    if (!impl_) {
        return {};
    }
    return impl_->snapshot();
}

std::size_t AgentSession::message_count() const { return impl_ ? impl_->message_count() : 0; }

std::optional<std::string> AgentSession::last_assistant_text() const {
    return impl_ ? impl_->last_assistant_text() : std::nullopt;
}

std::optional<std::string> AgentSession::session_name() const { return impl_ ? impl_->session_name() : std::nullopt; }

support::Expected<std::optional<std::string>> AgentSession::set_session_name(
    std::string name) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->set_session_name(std::move(name));
}

runtime::SessionStats AgentSession::session_stats() const {
    return impl_ ? impl_->session_stats() : runtime::SessionStats{};
}

const std::string& AgentSession::session_id() const {
    static const std::string empty;
    return impl_ ? impl_->session_id() : empty;
}

const std::optional<std::filesystem::path>& AgentSession::session_path() const {
    static const std::optional<std::filesystem::path> absent;
    return impl_ ? impl_->session_path_ : absent;
}

const std::string& AgentSession::provider() const {
    static const std::string empty;
    return impl_ ? impl_->provider() : empty;
}

const std::string& AgentSession::model() const {
    static const std::string empty;
    return impl_ ? impl_->model() : empty;
}

std::shared_ptr<ModelRuntime> AgentSession::model_runtime() const { return impl_ ? impl_->model_runtime() : nullptr; }

const std::filesystem::path& AgentSession::workspace() const {
    static const std::filesystem::path empty;
    return impl_ ? impl_->workspace() : empty;
}

void AgentSession::abort() {
    if (impl_) {
        impl_->abort();
    }
}

void AgentSession::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

bool AgentSession::is_open() const { return impl_ && impl_->is_open(); }

bool AgentSession::is_busy() const { return impl_ && impl_->is_busy(); }

bool AgentSession::is_streaming() const { return impl_ && impl_->is_streaming(); }

bool AgentSession::is_compacting() const { return impl_ && impl_->is_compacting(); }

const std::optional<std::string>& AgentSession::system_prompt_source() const {
    static const std::optional<std::string> empty;
    return impl_ ? impl_->system_prompt_source() : empty;
}

const std::vector<std::string>& AgentSession::append_system_prompt_sources() const {
    static const std::vector<std::string> empty;
    return impl_ ? impl_->append_system_prompt_sources() : empty;
}

const std::vector<prompt::ProjectContextFile>& AgentSession::context_files() const {
    static const std::vector<prompt::ProjectContextFile> empty;
    return impl_ ? impl_->context_files() : empty;
}

const std::vector<ResourceDiagnostic>& AgentSession::skill_diagnostics() const {
    static const std::vector<ResourceDiagnostic> empty;
    return impl_ ? impl_->skill_diagnostics() : empty;
}

const std::vector<ResourceDiagnostic>& AgentSession::prompt_diagnostics() const {
    static const std::vector<ResourceDiagnostic> empty;
    return impl_ ? impl_->prompt_diagnostics() : empty;
}

const std::vector<ResourceDiagnostic>& AgentSession::theme_diagnostics() const {
    static const std::vector<ResourceDiagnostic> empty;
    return impl_ ? impl_->theme_diagnostics() : empty;
}

const std::vector<Skill>& AgentSession::skills() const {
    static const std::vector<Skill> empty;
    return impl_ ? impl_->skills() : empty;
}

const std::vector<PromptTemplate>& AgentSession::templates() const {
    static const std::vector<PromptTemplate> empty;
    return impl_ ? impl_->templates() : empty;
}

namespace {
/// The fork source facts for the current session: the persisted file, or
/// the in-memory store's live tree when the session has no file.
[[nodiscard]] runtime::ForkSource current_fork_source(const AgentSession::Impl& impl) {
    runtime::ForkSource source;
    source.workspace = impl.session_.workspace;
    const auto& store = impl.session_.store;
    const auto path = store ? store->path() : std::nullopt;
    if (path.has_value()) {
        source.source = runtime::PersistedForkSource{.session_path = *path};
    } else {
        source.source = runtime::InMemoryForkSource{.store = store};
    }
    return source;
}
} // namespace

std::vector<runtime::UserForkMessage> AgentSession::get_user_messages_for_forking() const {
    if (!impl_) {
        return {};
    }
    return runtime::user_messages_for_forking(current_fork_source(*impl_));
}

support::Expected<runtime::ForkPreparation> AgentSession::prepare_fork(
    std::string_view entry_id,
    runtime::ForkPosition position) const {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Session,
            "Invalid entry ID for forking"));
    }
    return runtime::prepare_fork(
        current_fork_source(*impl_), entry_id, position);
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::wait_for_idle() {
    return detail::session_wait_for_idle(impl_);
}

support::Expected<SessionTreeTopology> AgentSession::session_tree() const {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->session_tree();
}

support::Expected<TreeNavigationResult> AgentSession::navigate_tree(
    std::string_view target_id) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->navigate_tree(target_id);
}

support::ExpectedVoid AgentSession::set_entry_label(
    std::string_view entry_id,
    std::optional<std::string> label) {
    if (!impl_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->set_entry_label(entry_id, std::move(label));
}

boost::asio::awaitable<support::Expected<AgentSessionReloadResult>> AgentSession::reload(std::stop_token stop_token) {
    // Same impl_ copying contract as prompt()/set_model: the impl_ copy
    // enters the session_reload frame synchronously at this call, so moving
    // or destroying the public handle before the first co_await cannot
    // invalidate the returned lazy awaitable.
    return detail::session_reload(impl_, stop_token);
}

// ─────────────────────────────────────────────────────────────────────────────
// Session Assembly publication
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<AgentSession> AgentSession::bind_assembly(runtime::AgentSessionAssembly assembly) {
    auto session = std::make_unique<AgentSession>();
    session->impl_ = std::make_shared<AgentSession::Impl>(std::move(assembly));
    return session;
}

support::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request) {
    return runtime::SessionFactory::create(std::move(request), std::nullopt);
}

support::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request,
    std::optional<runtime::InteractiveSessionFacts> session_facts,
    runtime::AssemblyOverrides overrides) {
    return runtime::SessionFactory::create(
        std::move(request), std::move(session_facts), std::move(overrides));
}

support::AsyncResult<CreateAgentSessionResult> create_agent_session_async(runtime::AgentSessionCreationRequest request,
        std::optional<runtime::InteractiveSessionFacts> session_facts,
        runtime::AssemblyOverrides overrides,
        std::stop_token stop_token) {
    return runtime::SessionFactory::create_async(
            std::move(request), std::move(session_facts), std::move(overrides), stop_token);
}

support::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models) {
    return runtime::SessionFactory::create(std::move(request),
            std::nullopt,
            runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .cli_fake = false, .models = std::move(models), .user_shell = nullptr});
}

support::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<runtime::AsyncUserShell> user_shell) {
    return runtime::SessionFactory::create(std::move(request),
            std::nullopt,
            runtime::AssemblyOverrides{.model_runtime = nullptr,
                    .cli_fake = false,
                    .models = std::move(models),
                    .user_shell = std::move(user_shell)});
}

} // namespace cch::coding_agent
