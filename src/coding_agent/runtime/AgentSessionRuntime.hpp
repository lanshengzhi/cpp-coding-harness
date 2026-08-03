#pragma once

#include <cch/agent/Agent.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/prompt/PromptProcessor.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionEventCommitment.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"
#include "coding_agent/runtime/UserBash.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct AgentSessionRuntimeConfig {
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// Explicit turn cap forwarded to the Agent; std::nullopt imposes no cap
    /// (ADR 0015).
    std::optional<int> max_turns{std::nullopt};
    ai::Model model{};
};

/// Internal runtime behind AgentSession. Composes the stateful Agent with
/// session persistence, prompt processing, resources, and SDK presentation.
class AgentSessionRuntime {
public:
    AgentSessionRuntime(
        RuntimeServices services,
        OpenSession session,
        prompt::PromptProcessor prompt_processor,
        AgentSessionRuntimeConfig config);

    AgentSessionRuntime(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime& operator=(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime(AgentSessionRuntime&&) = delete;
    AgentSessionRuntime& operator=(AgentSessionRuntime&&) = delete;

    /// Run one prompt on the awaiting host executor through optional prompt
    /// interpretation, the stateful Agent, persistence, and event fanout.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_prompt(
        std::string prompt,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted = {});

    /// Private Native TUI path for one direct-user Shell execution. User Bash
    /// may overlap an active Agent run; a result completed mid-run stays
    /// pending and commits exactly once after the whole run settles.
    [[nodiscard]] bool has_user_shell() const { return services_.user_shell != nullptr; }
    [[nodiscard]] boost::asio::awaitable<util::Expected<UserBashCompletion>> run_user_bash(
        std::string command,
        bool exclude_from_context,
        UserBashProgressSink progress_sink);
    void cancel_user_bash();

    // ── Subscriptions ──────────────────────────────────────────────────────

    /// Subscribe through the authoritative stateful Agent weak-observer path.
    [[nodiscard]] util::Expected<agent::AgentEventSubscription> subscribe(
        agent::AgentEventSink sink);

    // ── Input queues ───────────────────────────────────────────────────────

    [[nodiscard]] util::ExpectedVoid steer(
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates);
    [[nodiscard]] util::ExpectedVoid follow_up(
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates);
    [[nodiscard]] util::ExpectedVoid set_steering_mode(agent::InputQueueMode mode);
    [[nodiscard]] util::ExpectedVoid set_follow_up_mode(agent::InputQueueMode mode);
    [[nodiscard]] util::ExpectedVoid clear_steering_queue();
    [[nodiscard]] util::ExpectedVoid clear_follow_up_queue();
    [[nodiscard]] util::ExpectedVoid clear_input_queues();

    // ── State accessors ────────────────────────────────────────────────────

    [[nodiscard]] AgentSessionSnapshot snapshot(
        const std::optional<std::filesystem::path>& session_path) const;
    [[nodiscard]] std::size_t message_count() const;
    [[nodiscard]] std::optional<std::string> last_assistant_text() const;
    [[nodiscard]] const std::string& session_id() const { return session_.metadata.session_id; }
    [[nodiscard]] std::optional<std::filesystem::path> session_path() const {
        return session_.store ? session_.store->path() : std::nullopt;
    }
    [[nodiscard]] const std::string& provider() const { return session_.metadata.provider; }
    [[nodiscard]] const std::string& model() const { return session_.metadata.model; }
    [[nodiscard]] std::shared_ptr<ModelRuntime> model_runtime() const {
        return services_.model_runtime;
    }
    [[nodiscard]] const std::filesystem::path& workspace() const { return session_.workspace; }
    [[nodiscard]] const std::vector<Skill>& skills() const;
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Request cancellation of the active Agent prompt. Idempotent and a
    /// no-op while idle.
    void abort();

    [[nodiscard]] bool is_open() const {
        return lifecycle_ == Lifecycle::Open;
    }
    [[nodiscard]] bool is_busy() const {
        return lifecycle_ == Lifecycle::Closing || prompt_active_ ||
            user_bash_active_;
    }
    void close() noexcept;

private:
    /// Session lifecycle, tracked independently from the active-work facts so
    /// User Bash may overlap an Agent run (ADR 0026).
    enum class Lifecycle { Open, Closing, Closed };

    /// One completed User Bash execution whose commitment is deferred until
    /// the active Agent run settles. The signal is cancelled by the flush to
    /// release the awaiting run_user_bash coroutine, which then returns the
    /// completion or the commitment failure.
    struct PendingUserBashCommit {
        UserBashCompletion completion;
        boost::asio::steady_timer committed_signal;
        util::ExpectedVoid commit_result;
    };

    /// Shared preflight outcome for entry points that require a non-closed session.
    [[nodiscard]] util::ExpectedVoid reject_if_closed() const;
    /// Shared preflight outcome for entry points that reject a concurrent prompt.
    [[nodiscard]] util::ExpectedVoid reject_if_busy() const;
    /// Shared preflight outcome rejecting a second concurrent User Bash.
    [[nodiscard]] util::ExpectedVoid reject_if_user_bash_busy() const;

    /// Commit one completed Bash message to Live Session State, then the
    /// Session Store. Store failure is reported on the completion diagnostic
    /// without rolling back Live Session State; a Live Session State failure
    /// is returned so the caller can reject the completion outright.
    [[nodiscard]] util::ExpectedVoid commit_user_bash_completion(
        UserBashCompletion& completion);
    /// Commit and release every deferred Bash completion in completion order.
    void flush_pending_user_bash();

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_agent_loop(
        ai::UserMessage prompt,
        std::stop_source stop_source);
    [[nodiscard]] boost::asio::awaitable<void> finalize_close_after_active_work();
    [[nodiscard]] std::shared_ptr<harness::AsyncExecutionEnv> release_close_resources() noexcept;
    void finalize_close() noexcept;

    RuntimeServices services_;
    OpenSession session_;
    std::optional<prompt::PromptProcessor> prompt_processor_;
    // Declared after the borrowed client/store owners so it is destroyed first.
    std::optional<agent::Agent> agent_;

    AgentSessionRuntimeConfig config_;
    Lifecycle lifecycle_{Lifecycle::Open};
    bool prompt_active_{false};
    bool user_bash_active_{false};
    std::vector<std::shared_ptr<PendingUserBashCommit>> pending_user_bash_;
    std::optional<std::stop_source> active_stop_source_;
    std::optional<std::stop_source> active_user_bash_stop_source_;
};

} // namespace cch::coding_agent::runtime
