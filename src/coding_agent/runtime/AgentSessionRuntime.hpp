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

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct AgentSessionRuntimeConfig {
    /// Explicit turn cap forwarded to the Agent; std::nullopt imposes no cap
    /// (ADR 0015).
    std::optional<int> max_turns{std::nullopt};
    std::string model;
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
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted = {});

    // ── Subscriptions ──────────────────────────────────────────────────────

    /// Subscribe through the authoritative stateful Agent weak-observer path.
    [[nodiscard]] util::Expected<agent::AgentEventSubscription> subscribe(
        agent::AgentEventSink sink);

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
    [[nodiscard]] const std::filesystem::path& workspace() const { return session_.workspace; }
    [[nodiscard]] const std::vector<Skill>& skills() const;
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Request cancellation of the active Agent prompt. Idempotent and a
    /// no-op while idle.
    void abort();

    [[nodiscard]] bool is_open() const {
        return state_ == State::Open || state_ == State::RunningPrompt;
    }
    [[nodiscard]] bool is_busy() const {
        return state_ == State::RunningPrompt || state_ == State::Closing;
    }
    void close() noexcept;

private:
    enum class State { Open, RunningPrompt, Closing, Closed };

    /// Shared preflight outcome for entry points that require a non-closed session.
    [[nodiscard]] util::ExpectedVoid reject_if_closed() const;
    /// Shared preflight outcome for entry points that reject a concurrent prompt.
    [[nodiscard]] util::ExpectedVoid reject_if_busy() const;

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_agent_loop(
        std::string prompt,
        std::stop_source stop_source);
    [[nodiscard]] boost::asio::awaitable<void> finalize_close_after_prompt();
    [[nodiscard]] std::shared_ptr<harness::AsyncExecutionEnv> release_close_resources() noexcept;
    void finalize_close() noexcept;

    RuntimeServices services_;
    OpenSession session_;
    std::optional<prompt::PromptProcessor> prompt_processor_;
    // Declared after the borrowed client/store owners so it is destroyed first.
    std::optional<agent::Agent> agent_;

    AgentSessionRuntimeConfig config_;
    State state_{State::Open};
    std::optional<std::stop_source> active_stop_source_;
};

} // namespace cch::coding_agent::runtime
