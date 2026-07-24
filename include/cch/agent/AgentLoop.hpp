#pragma once

#include "AgentContext.hpp"
#include "AgentEvent.hpp"
#include "ToolRegistry.hpp"

#include "../ai/ChatClient.hpp"

#include <boost/asio/awaitable.hpp>

#include <string>
#include <vector>

namespace cch::agent {

class AsyncAgentLoop {
public:
    AsyncAgentLoop(ai::StreamingChatClient& client, AsyncToolRegistry registry, AsyncAgentOptions options = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> run(
        std::string user_prompt,
        AgentEventSink sink = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        std::string user_prompt,
        AgentEventSink sink = {});

private:
    friend class Agent;

    [[nodiscard]] const std::string& current_model() const noexcept {
        return options_.model;
    }
    [[nodiscard]] const std::string& current_thinking_level() const noexcept {
        return options_.thinking_level;
    }

    [[nodiscard]] util::ExpectedVoid append_message_with_lifecycle(
        AgentState& state,
        ai::AiContext& context,
        AgentEventSink& sink,
        ai::MessageVariant message) const;
    [[nodiscard]] util::ExpectedVoid emit(AgentEventSink& sink, const AgentLifecycleEvent& event) const;
    [[nodiscard]] std::vector<ai::ToolCallContent> tool_calls(const ai::AssistantMessage& message) const;

    ai::StreamingChatClient& client_;
    AsyncToolRegistry registry_;
    AsyncAgentOptions options_;
};

} // namespace cch::agent
