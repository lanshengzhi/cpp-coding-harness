#pragma once

#include "Content.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::ai {

enum class MessageRole {
    System,
    User,
    Assistant,
    ToolResult,
};

inline std::string to_string(MessageRole role) {
    switch (role) {
    case MessageRole::System:
        return "system";
    case MessageRole::User:
        return "user";
    case MessageRole::Assistant:
        return "assistant";
    case MessageRole::ToolResult:
        return "toolResult";
    }
    return "user";
}

inline std::optional<MessageRole> role_from_string(const std::string& role) {
    if (role == "system") {
        return MessageRole::System;
    }
    if (role == "user") {
        return MessageRole::User;
    }
    if (role == "assistant") {
        return MessageRole::Assistant;
    }
    if (role == "toolResult" || role == "tool_result" || role == "tool") {
        return MessageRole::ToolResult;
    }
    return std::nullopt;
}

struct Message {
    MessageRole role{MessageRole::User};
    std::vector<ContentBlock> content;
    std::string tool_call_id;
    std::string tool_name;
    bool is_error{false};

    static Message user_text(std::string text) {
        Message message;
        message.role = MessageRole::User;
        message.content.push_back(ContentBlock::from_text(std::move(text)));
        return message;
    }

    static Message assistant_text(std::string text) {
        Message message;
        message.role = MessageRole::Assistant;
        message.content.push_back(ContentBlock::from_text(std::move(text)));
        return message;
    }

    static Message tool_result(std::string call_id, std::string name, std::string text, bool error = false) {
        Message message;
        message.role = MessageRole::ToolResult;
        message.tool_call_id = std::move(call_id);
        message.tool_name = std::move(name);
        message.is_error = error;
        message.content.push_back(ContentBlock::from_text(std::move(text)));
        return message;
    }
};

inline std::string text_content(const Message& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (block.kind == ContentKind::Text) {
            text += block.text;
        }
    }
    return text;
}

inline std::vector<ToolCall> tool_calls(const Message& message) {
    std::vector<ToolCall> calls;
    for (const auto& block : message.content) {
        if (block.kind == ContentKind::ToolCall) {
            calls.push_back(block.tool_call);
        }
    }
    return calls;
}

} // namespace cch::ai
