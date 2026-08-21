#include "coding_agent/AgentSession.hpp"

#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/AgentSessionPromptAccess.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include "ai/AsyncResultBridge.hpp"

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

boost::asio::awaitable<support::ExpectedVoid> AgentSession::prompt(
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

support::ExpectedVoid AgentSession::prompt_blocking(
    std::string text,
    PromptOptions options) {
    return detail::AgentSessionPromptAccess::prompt_blocking(
        *this,
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates,
        {});
}

support::ExpectedVoid AgentSession::steer(
    std::string text,
    PromptOptions options) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->steer(
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates);
}

support::ExpectedVoid AgentSession::follow_up(
    std::string text,
    PromptOptions options) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->follow_up(
        std::move(text),
        std::move(options.images),
        options.expand_prompt_templates);
}

support::ExpectedVoid AgentSession::set_steering_mode(agent::InputQueueMode mode) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_steering_mode(mode);
}

support::ExpectedVoid AgentSession::set_follow_up_mode(agent::InputQueueMode mode) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_follow_up_mode(mode);
}

support::ExpectedVoid AgentSession::clear_steering_queue() {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->clear_steering_queue();
}

support::ExpectedVoid AgentSession::clear_follow_up_queue() {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->clear_follow_up_queue();
}

support::ExpectedVoid AgentSession::clear_input_queues() {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->clear_input_queues();
}

support::Expected<std::string> AgentSession::set_thinking_level(
    std::string_view level) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_thinking_level(level);
}

boost::asio::awaitable<support::Expected<CompactionResult>> AgentSession::compact(
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

bool detail::AgentSessionInteractiveAccess::is_project_trusted(
    const AgentSession& session) {
    return session.impl_ && session.impl_->runtime &&
        session.impl_->runtime->is_project_trusted();
}

boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>>
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

boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>>
detail::AgentSessionInteractiveAccess::run_user_bash_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string command,
    bool exclude_from_context,
    runtime::UserBashProgressSink progress_sink) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

boost::asio::awaitable<support::ExpectedVoid> detail::AgentSessionPromptAccess::prompt(
    AgentSession& session,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<support::ExpectedVoid()> on_preflight_accepted) {
    return prompt_impl(
        session.impl_,
        std::move(text),
        std::move(images),
        expand_prompt_templates,
        std::move(on_preflight_accepted));
}

boost::asio::awaitable<support::ExpectedVoid> detail::AgentSessionPromptAccess::prompt_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<support::ExpectedVoid()> on_preflight_accepted) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->runtime->run_prompt(
            std::move(text),
            std::move(images),
            expand_prompt_templates,
            std::move(on_preflight_accepted));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session prompt coroutine failed",
            error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session prompt coroutine failed"));
    }
#endif
}

boost::asio::awaitable<support::ExpectedVoid>
detail::AgentSessionPromptAccess::wait_for_idle(
    AgentSession& session) {
    return wait_for_idle_impl(session.impl_);
}

boost::asio::awaitable<support::ExpectedVoid>
detail::AgentSessionPromptAccess::wait_for_idle_impl(
    std::shared_ptr<AgentSession::Impl> impl) {
    if (impl && impl->runtime) {
        co_await impl->runtime->wait_for_idle();
    }
    co_return support::ExpectedVoid{};
}

boost::asio::awaitable<support::Expected<CompactionResult>>
detail::AgentSessionPromptAccess::compact(
    AgentSession& session,
    std::string custom_instructions) {
    return compact_impl(session.impl_, std::move(custom_instructions));
}

boost::asio::awaitable<support::Expected<CompactionResult>>
detail::AgentSessionPromptAccess::compact_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string custom_instructions) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->runtime->compact(std::move(custom_instructions));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session compact coroutine failed",
            error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session compact coroutine failed"));
    }
#endif
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::set_model(
    ai::Model model) {
    return detail::AgentSessionPromptAccess::set_model(*this, std::move(model));
}

support::ExpectedVoid AgentSession::set_model_blocking(ai::Model model) {
    return detail::AgentSessionPromptAccess::set_model_blocking(
        *this, std::move(model));
}

boost::asio::awaitable<support::ExpectedVoid>
detail::AgentSessionPromptAccess::set_model(
    AgentSession& session,
    ai::Model model) {
    return set_model_impl(session.impl_, std::move(model));
}

boost::asio::awaitable<support::ExpectedVoid>
detail::AgentSessionPromptAccess::set_model_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    ai::Model model) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->runtime->set_model(std::move(model));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session set_model coroutine failed",
            error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session set_model coroutine failed"));
    }
#endif
}

support::ExpectedVoid detail::AgentSessionPromptAccess::set_model_blocking(
    AgentSession& session,
    ai::Model model) {
    const auto impl = session.impl_;
    if (!impl) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    // Run the lazy coroutine on the temporary executor through the private
    // completion bridge: the bridge owns the Asio `co_spawn` completion and
    // maps its staged-build exception pointer to the operation's typed
    // outcome, so no exception-shaped code survives outside the bridge.
    auto bridged = ai::detail::make_async_result_on(
        io.get_executor(),
        [&session, model = std::move(model)]() mutable
            -> boost::asio::awaitable<support::ExpectedVoid> {
            co_return co_await set_model(session, std::move(model));
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
    return detail::AgentSessionPromptAccess::cycle_model(*this, std::move(direction));
}

support::Expected<std::optional<ModelCycleResult>> AgentSession::cycle_model_blocking(
    std::string direction) {
    return detail::AgentSessionPromptAccess::cycle_model_blocking(
        *this, std::move(direction));
}

boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>>
detail::AgentSessionPromptAccess::cycle_model(
    AgentSession& session,
    std::string direction) {
    return cycle_model_impl(session.impl_, std::move(direction));
}

boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>>
detail::AgentSessionPromptAccess::cycle_model_impl(
    std::shared_ptr<AgentSession::Impl> impl,
    std::string direction) {
    if (!impl || !impl->runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->runtime->cycle_model(std::move(direction));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session cycle_model coroutine failed",
            error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session cycle_model coroutine failed"));
    }
#endif
}

support::Expected<std::optional<ModelCycleResult>>
detail::AgentSessionPromptAccess::cycle_model_blocking(
    AgentSession& session,
    std::string direction) {
    const auto impl = session.impl_;
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
    auto bridged = ai::detail::make_async_result_on(
        io.get_executor(),
        [&session, direction = std::move(direction)]() mutable
            -> boost::asio::awaitable<
                support::Expected<std::optional<ModelCycleResult>>> {
            co_return co_await cycle_model(session, std::move(direction));
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
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->cycle_thinking_level();
}

void AgentSession::set_scoped_models(std::vector<ScopedModel> models) {
    if (impl_ && impl_->runtime) {
        impl_->runtime->set_scoped_models(std::move(models));
    }
}

const std::vector<ScopedModel>& AgentSession::scoped_models() const {
    static const std::vector<ScopedModel> kEmpty;
    return impl_ && impl_->runtime ? impl_->runtime->scoped_models() : kEmpty;
}

support::ExpectedVoid detail::AgentSessionPromptAccess::prompt_blocking(
    AgentSession& session,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<support::ExpectedVoid()> on_preflight_accepted) {
    const auto impl = session.impl_;
    if (!impl) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    if (blocking_prompt_wait == impl.get()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    BlockingPromptWaitScope wait_scope{impl.get()};

    // Same private completion bridge as set_model_blocking / cycle_model_blocking.
    auto bridged = ai::detail::make_async_result_on(
        io.get_executor(),
        [&session,
         text = std::move(text),
         images = std::move(images),
         expand_prompt_templates,
         on_preflight_accepted = std::move(on_preflight_accepted)]() mutable
            -> boost::asio::awaitable<support::ExpectedVoid> {
            co_return co_await prompt(
                session,
                std::move(text),
                std::move(images),
                expand_prompt_templates,
                std::move(on_preflight_accepted));
        });
    std::move(bridged).start([&result](support::ExpectedVoid completion) noexcept {
        result.emplace(std::move(completion));
    });
    io.run();

    if (!result) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session prompt coroutine did not complete"));
    }
    return std::move(*result);
}

support::Expected<EventSubscription> AgentSession::subscribe(agent::AgentEventSink sink) {
    if (!impl_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
    if (!sink) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "event sink is empty"));
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

std::optional<std::string> AgentSession::session_name() const {
    return impl_ && impl_->runtime ? impl_->runtime->session_name() : std::nullopt;
}

support::Expected<std::optional<std::string>> AgentSession::set_session_name(
    std::string name) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_session_name(std::move(name));
}

runtime::SessionStats AgentSession::session_stats() const {
    return impl_ && impl_->runtime ? impl_->runtime->session_stats()
                                   : runtime::SessionStats{};
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

bool AgentSession::is_streaming() const {
    return impl_ && impl_->runtime && impl_->runtime->is_streaming();
}

bool AgentSession::is_compacting() const {
    return impl_ && impl_->runtime && impl_->runtime->is_compacting();
}

const std::optional<std::string>& AgentSession::system_prompt_source() const {
    static const std::optional<std::string> empty;
    return impl_ && impl_->runtime ? impl_->runtime->system_prompt_source() : empty;
}

const std::vector<std::string>& AgentSession::append_system_prompt_sources() const {
    static const std::vector<std::string> empty;
    return impl_ && impl_->runtime ? impl_->runtime->append_system_prompt_sources() : empty;
}

const std::vector<prompt::ProjectContextFile>& AgentSession::context_files() const {
    static const std::vector<prompt::ProjectContextFile> empty;
    return impl_ && impl_->runtime ? impl_->runtime->context_files() : empty;
}

const std::vector<ResourceDiagnostic>& AgentSession::skill_diagnostics() const {
    static const std::vector<ResourceDiagnostic> empty;
    return impl_ && impl_->runtime ? impl_->runtime->skill_diagnostics() : empty;
}

const std::vector<ResourceDiagnostic>& AgentSession::prompt_diagnostics() const {
    static const std::vector<ResourceDiagnostic> empty;
    return impl_ && impl_->runtime ? impl_->runtime->prompt_diagnostics() : empty;
}

const std::vector<ResourceDiagnostic>& AgentSession::theme_diagnostics() const {
    static const std::vector<ResourceDiagnostic> empty;
    return impl_ && impl_->runtime ? impl_->runtime->theme_diagnostics() : empty;
}

const std::vector<Skill>& AgentSession::skills() const {
    static const std::vector<Skill> empty;
    return impl_ && impl_->runtime ? impl_->runtime->skills() : empty;
}

const std::vector<PromptTemplate>& AgentSession::templates() const {
    static const std::vector<PromptTemplate> empty;
    return impl_ && impl_->runtime ? impl_->runtime->templates() : empty;
}

namespace {
/// The fork source facts for the current session: the persisted file, or
/// the in-memory store's live tree when the session has no file.
[[nodiscard]] runtime::ForkSource current_fork_source(const AgentSession::Impl& impl) {
    runtime::ForkSource source;
    source.workspace = impl.runtime->workspace();
    if (const auto path = impl.runtime->session_path(); path.has_value()) {
        source.source = runtime::PersistedForkSource{.session_path = *path};
    } else {
        source.source =
            runtime::InMemoryForkSource{.store = impl.runtime->session_store()};
    }
    return source;
}
} // namespace

std::vector<runtime::UserForkMessage> AgentSession::get_user_messages_for_forking() const {
    if (!impl_ || !impl_->runtime) {
        return {};
    }
    return runtime::user_messages_for_forking(current_fork_source(*impl_));
}

support::Expected<runtime::ForkPreparation> AgentSession::prepare_fork(
    std::string_view entry_id,
    runtime::ForkPosition position) const {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Session,
            "Invalid entry ID for forking"));
    }
    return runtime::prepare_fork(
        current_fork_source(*impl_), entry_id, position);
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::wait_for_idle() {
    return detail::AgentSessionPromptAccess::wait_for_idle(*this);
}

support::Expected<SessionTreeTopology> AgentSession::session_tree() const {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->session_tree();
}

support::Expected<TreeNavigationResult> AgentSession::navigate_tree(
    std::string_view target_id) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->navigate_tree(target_id);
}

support::ExpectedVoid AgentSession::set_entry_label(
    std::string_view entry_id,
    std::optional<std::string> label) {
    if (!impl_ || !impl_->runtime) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is not initialized"));
    }
    return impl_->runtime->set_entry_label(entry_id, std::move(label));
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

class AgentSessionRuntimeAccess {
public:
    [[nodiscard]] static boost::asio::awaitable<support::Expected<runtime::AgentSessionReloadResult>>
    reload(AgentSession& session) {
        return reload_impl(session.impl_);
    }

    [[nodiscard]] static boost::asio::awaitable<support::Expected<runtime::AgentSessionReloadResult>>
    reload_impl(std::shared_ptr<AgentSession::Impl> impl) {
        if (!impl || !impl->runtime) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "session is not initialized"));
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            co_return co_await impl->runtime->reload();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "session reload coroutine failed",
                error.what()));
        } catch (...) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "session reload coroutine failed"));
        }
#endif
    }

};

} // namespace detail

boost::asio::awaitable<support::Expected<runtime::AgentSessionReloadResult>>
AgentSession::reload() {
    // Same impl_ copying contract as prompt()/set_model: session.impl_ is
    // copied into the reload frame synchronously at the call, so moving or
    // destroying the public handle before the first co_await cannot
    // invalidate the returned lazy awaitable.
    return detail::AgentSessionRuntimeAccess::reload(*this);
}

std::unique_ptr<AgentSession> AgentSession::bind_runtime(
    std::unique_ptr<runtime::AgentSessionRuntime> runtime,
    std::optional<std::filesystem::path> session_path) {
    auto session = std::make_unique<AgentSession>();
    session->impl_ = std::make_shared<AgentSession::Impl>();
    session->impl_->session_path = std::move(session_path);
    session->impl_->runtime = std::move(runtime);
    return session;
}

support::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request) {
    return runtime::SessionFactory::create(std::move(request));
}

support::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request,
    runtime::AssemblyOverrides overrides) {
    return runtime::SessionFactory::create(std::move(request), std::move(overrides));
}

support::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models) {
    return runtime::SessionFactory::create(
        std::move(request),
        runtime::AssemblyOverrides{
            .models = std::move(models),
            .user_shell = nullptr});
}

support::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<runtime::AsyncUserShell> user_shell) {
    return runtime::SessionFactory::create(
        std::move(request),
        runtime::AssemblyOverrides{
            .models = std::move(models),
            .user_shell = std::move(user_shell)});
}

} // namespace cch::coding_agent
