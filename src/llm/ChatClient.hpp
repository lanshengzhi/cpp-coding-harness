#pragma once

#include "../agent/Message.hpp"
#include "../agent/Tool.hpp"
#include "../ai/ChatClient.hpp"
#include "../util/Result.hpp"

#include <string>
#include <vector>

namespace cch::llm {

struct ChatRequest {
    std::vector<agent::Message> messages;
    std::vector<agent::ToolDefinition> tools;
    std::string model;
};

struct ChatResponse {
    agent::Message assistant_message;
    std::string stop_reason{"stop"};
};

class ChatClient {
public:
    virtual ~ChatClient() = default;
    [[nodiscard]] virtual util::Result<ChatResponse> complete(const ChatRequest& request) = 0;
};

inline ai::ChatRequest to_ai_chat_request(const ChatRequest& request) {
    ai::ChatRequest converted;
    converted.context.model = request.model;
    converted.context.messages.reserve(request.messages.size());
    for (const auto& message : request.messages) {
        converted.context.messages.push_back(agent::to_ai_message(message));
    }
    converted.context.tools.reserve(request.tools.size());
    for (const auto& tool : request.tools) {
        converted.context.tools.push_back(agent::to_ai_tool_definition(tool));
    }
    return converted;
}

inline ChatRequest chat_request_from_ai(const ai::ChatRequest& request) {
    ChatRequest converted;
    converted.model = request.context.model;
    converted.messages.reserve(request.context.messages.size());
    for (const auto& message : request.context.messages) {
        converted.messages.push_back(agent::message_from_ai(message));
    }
    converted.tools.reserve(request.context.tools.size());
    for (const auto& tool : request.context.tools) {
        converted.tools.push_back(agent::tool_definition_from_ai(tool));
    }
    return converted;
}

inline ai::ChatResponse to_ai_chat_response(const ChatResponse& response) {
    ai::ChatResponse converted;
    converted.assistant_message = agent::to_ai_message(response.assistant_message);
    converted.stop_reason = ai::stop_reason_from_string(response.stop_reason);
    return converted;
}

inline ChatResponse chat_response_from_ai(const ai::ChatResponse& response) {
    ChatResponse converted;
    converted.assistant_message = agent::message_from_ai(response.assistant_message);
    converted.stop_reason = ai::to_string(response.stop_reason);
    if (response.stop_reason == ai::StopReason::ToolUse) {
        converted.stop_reason = "tool_calls";
    }
    return converted;
}

} // namespace cch::llm
