#pragma once

#include "../../../include/cch/agent/AgentLoop.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/util/Error.hpp"

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
                       agent::AsyncAgentOptions options);

    [[nodiscard]] PromptRunResult run_prompt(
        std::vector<ai::MessageVariant>& history,
        harness::session::JsonlSessionStore& store,
        std::string prompt,
        agent::AgentEventSink sink = {});

private:
    agent::AsyncAgentLoop loop_;
};

[[nodiscard]] std::string terminal_code_for_loop_error(const std::string& message);
[[nodiscard]] std::string display_message_for_loop_error(const std::string& message);

} // namespace cch::coding_agent::runtime
