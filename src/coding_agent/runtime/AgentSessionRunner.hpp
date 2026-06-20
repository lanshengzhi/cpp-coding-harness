#pragma once

#include "../../../include/cch/agent/AgentLoop.hpp"
#include "../../../include/cch/coding_agent/PromptProcessing.hpp"
#include "../../../include/cch/coding_agent/Skill.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct PromptRunResult {
    bool success{false};
    std::string code;
    std::string message;
};

class AgentSessionRunner {
public:
    AgentSessionRunner(ai::StreamingChatClient& client,
                       agent::AsyncToolRegistry registry,
                       agent::AsyncAgentOptions options,
                       std::vector<PromptTemplate> templates = {},
                       CommandRegistry* command_registry = nullptr,
                       std::vector<Skill> skills = {});

    [[nodiscard]] PromptRunResult run_prompt(
        std::vector<ai::MessageVariant>& history,
        harness::session::JsonlSessionStore& store,
        std::string prompt,
        agent::AgentEventSink sink = {});

    /// Access the loaded skills (for external callers like the REPL loop).
    [[nodiscard]] const std::vector<Skill>& skills() const { return skills_; }

private:
    std::optional<agent::AsyncAgentLoop> loop_;
    std::vector<PromptTemplate> templates_;
    CommandRegistry* command_registry_;
    std::vector<Skill> skills_;
};

[[nodiscard]] std::string terminal_code_for_loop_error(const std::string& message);
[[nodiscard]] std::string display_message_for_loop_error(const std::string& message);

} // namespace cch::coding_agent::runtime
