#pragma once

#include "AgentContext.hpp"
#include "AgentEvent.hpp"
#include "AgentTool.hpp"
#include "Message.hpp"
#include "ToolRegistry.hpp"
#include "../ai/ChatClient.hpp"
#include "../util/Result.hpp"

#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace cch::agent {

struct LoopEvent {
    std::string type;
    std::string detail;
};

struct LoopOptions {
    std::filesystem::path workspace{"."};
    std::string model{"fake-model"};
    int max_turns{8};
    bool bash_enabled{false};
    std::vector<std::string> secret_environment_names;
    std::function<void(const LoopEvent&)> on_event;
    std::function<void(const AgentEvent&)> on_agent_event;
    std::function<util::Result<void>(const Message&)> on_message;
};

struct LoopResult {
    std::string final_text;
    std::vector<Message> messages;
    std::vector<LoopEvent> events;
    std::vector<AgentEvent> agent_events;
    std::string stop_reason;
};

class AgentLoop {
public:
    AgentLoop(ai::ChatClient& client, ToolRegistry registry, LoopOptions options = {});

    [[nodiscard]] util::Result<LoopResult> run(std::string user_prompt);
    [[nodiscard]] util::Result<LoopResult> continue_with(std::vector<Message> existing_history, std::string user_prompt);

private:
    void emit(std::vector<LoopEvent>& events, std::string type, std::string detail) const;
    void emit_agent(std::vector<AgentEvent>& events, AgentEvent event) const;
    util::Result<void> append(std::vector<Message>& messages, const Message& message) const;
    [[nodiscard]] Message execute_tool_call(const ToolCall& call);

    ai::ChatClient& client_;
    ToolRegistry registry_;
    LoopOptions options_;
};

} // namespace cch::agent
