#pragma once

#include "../../../include/cch/agent/Agent.hpp"
#include "../../../include/cch/coding_agent/PromptTemplate.hpp"
#include "../../../include/cch/coding_agent/Skill.hpp"
#include "../prompt/PromptProcessor.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "RuntimeServices.hpp"
#include "SessionEventCommitment.hpp"
#include "SessionLifecycle.hpp"

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

    /// Run one blocking prompt through optional prompt interpretation, the
    /// stateful Agent, persistence, and event fanout.
    [[nodiscard]] util::ExpectedVoid run_prompt(
        std::string prompt,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted = {});

    // ── Subscriptions ──────────────────────────────────────────────────────

    /// Subscribe through the authoritative stateful Agent weak-observer path.
    [[nodiscard]] util::Expected<agent::AgentEventSubscription> subscribe(
        agent::AgentEventSink sink);

    // ── State accessors ────────────────────────────────────────────────────

    [[nodiscard]] std::size_t message_count() const;
    [[nodiscard]] std::optional<std::string> last_assistant_text() const;
    [[nodiscard]] const std::string& session_id() const { return session_.metadata.session_id; }
    [[nodiscard]] std::optional<std::filesystem::path> session_path() const { return session_.store->path(); }
    [[nodiscard]] const std::string& provider() const { return session_.metadata.provider; }
    [[nodiscard]] const std::string& model() const { return session_.metadata.model; }
    [[nodiscard]] const std::filesystem::path& workspace() const { return session_.workspace; }
    [[nodiscard]] const std::vector<Skill>& skills() const { return prompt_processor_.skills(); }
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const { return prompt_processor_.templates(); }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    [[nodiscard]] bool is_open() const {
        return state_ == State::Open || state_ == State::RunningPrompt;
    }
    void close();

private:
    enum class State { Open, RunningPrompt, Closing, Closed };

    [[nodiscard]] util::ExpectedVoid run_agent_loop(std::string prompt);
    void finalize_close();

    RuntimeServices services_;
    OpenSession session_;
    prompt::PromptProcessor prompt_processor_;
    // Declared after the borrowed client/store owners so it is destroyed first.
    std::optional<agent::Agent> agent_;

    AgentSessionRuntimeConfig config_;
    State state_{State::Open};
};

} // namespace cch::coding_agent::runtime
