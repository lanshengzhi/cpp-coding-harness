#pragma once

#include "Message.hpp"
#include "ToolRegistry.hpp"
#include "../llm/ChatClient.hpp"
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
    std::function<void(const LoopEvent&)> on_event;
    std::function<util::Result<void>(const Message&)> on_message;
};

struct LoopResult {
    std::string final_text;
    std::vector<Message> messages;
    std::vector<LoopEvent> events;
    std::string stop_reason;
};

class AgentLoop {
public:
    AgentLoop(llm::ChatClient& client, ToolRegistry registry, LoopOptions options = {});

    [[nodiscard]] util::Result<LoopResult> run(std::string user_prompt);
    [[nodiscard]] util::Result<LoopResult> continue_with(std::vector<Message> existing_history, std::string user_prompt);

private:
    void emit(std::vector<LoopEvent>& events, std::string type, std::string detail) const;
    util::Result<void> append(std::vector<Message>& messages, const Message& message) const;
    [[nodiscard]] Message execute_tool_call(const ToolCall& call) const;

    llm::ChatClient& client_;
    ToolRegistry registry_;
    LoopOptions options_;
};

} // namespace cch::agent
