#pragma once

#include "Message.hpp"
#include "Tool.hpp"
#include "../ai/ChatClient.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cch::agent {

struct AgentContext {
    ai::Context model_context;

    static AgentContext from_legacy(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        std::string model) {
        AgentContext context;
        context.model_context.model = std::move(model);
        context.model_context.messages.reserve(messages.size());
        for (const auto& message : messages) {
            context.model_context.messages.push_back(to_ai_message(message));
        }
        context.model_context.tools.reserve(tools.size());
        for (const auto& tool : tools) {
            context.model_context.tools.push_back(to_ai_tool_definition(tool));
        }
        return context;
    }

    [[nodiscard]] ai::ChatRequest chat_request() const {
        ai::ChatRequest request;
        request.context = model_context;
        return request;
    }
};

} // namespace cch::agent
