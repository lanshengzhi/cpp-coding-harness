#pragma once

#include "../agent/Message.hpp"
#include "../agent/Tool.hpp"
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

} // namespace cch::llm
