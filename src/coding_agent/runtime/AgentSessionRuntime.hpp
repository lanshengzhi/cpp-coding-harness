#pragma once

#include "../../../include/cch/agent/AgentLoop.hpp"
#include "../../../include/cch/coding_agent/PromptTemplate.hpp"
#include "../../../include/cch/coding_agent/Skill.hpp"
#include "coding_agent/prompt/PromptProcessor.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "RuntimeServices.hpp"
#include "SessionEventCommitment.hpp"
#include "SessionLifecycle.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct AgentSessionRuntimeConfig {
    int max_turns{30};
    std::string model;
};

/// Internal runtime behind AgentSession. Owns the agent loop, session store,
/// history, prompt processing, and event fanout for a single session.
class AgentSessionRuntime {
public:
    AgentSessionRuntime(
        RuntimeServices services,
        OpenSession session,
        prompt::PromptProcessor prompt_processor,
        AgentSessionRuntimeConfig config);

    AgentSessionRuntime(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime& operator=(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime(AgentSessionRuntime&&) = default;
    AgentSessionRuntime& operator=(AgentSessionRuntime&&) = default;

    /// Run one blocking prompt through optional prompt interpretation, the
    /// agent loop, persistence, and event fanout.
    [[nodiscard]] util::ExpectedVoid run_prompt(
        std::string prompt,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted = {});

    // ── Subscriptions ──────────────────────────────────────────────────────

    /// Subscribe to agent lifecycle events. Returns a non-negative id.
    [[nodiscard]] int subscribe(agent::AgentEventSink sink);

    /// Unsubscribe by id. Idempotent.
    void unsubscribe(int id);

    /// True if the subscription id is currently active.
    [[nodiscard]] bool is_subscribed(int id) const;

    // ── State accessors ────────────────────────────────────────────────────

    [[nodiscard]] std::size_t message_count() const { return session_.history.size(); }
    [[nodiscard]] std::optional<std::string> last_assistant_text() const;
    [[nodiscard]] const std::string& session_id() const { return session_.metadata.session_id; }
    [[nodiscard]] std::optional<std::filesystem::path> session_path() const { return session_.store->path(); }
    [[nodiscard]] const std::string& provider() const { return session_.metadata.provider; }
    [[nodiscard]] const std::string& model() const { return session_.metadata.model; }
    [[nodiscard]] const std::filesystem::path& workspace() const { return session_.workspace; }
    [[nodiscard]] const std::vector<ai::MessageVariant>& history() const { return session_.history; }
    [[nodiscard]] const std::vector<Skill>& skills() const { return prompt_processor_.skills(); }
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const { return prompt_processor_.templates(); }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    [[nodiscard]] bool is_open() const { return state_ != State::Closed; }
    void close();

private:
    enum class State { Open, RunningPrompt, Closed };

    [[nodiscard]] util::ExpectedVoid run_agent_loop(std::string prompt);

    RuntimeServices services_;
    OpenSession session_;
    prompt::PromptProcessor prompt_processor_;
    std::optional<agent::AsyncAgentLoop> loop_;

    // std::deque keeps SubscriberEntry references stable so a subscriber
    // that subscribes from inside an event callback cannot invalidate the
    // run-start snapshot held by SessionEventCommitment.
    std::deque<SubscriberEntry> subscribers_;
    int next_subscriber_id_{0};

    AgentSessionRuntimeConfig config_;
    State state_{State::Open};
};

} // namespace cch::coding_agent::runtime
