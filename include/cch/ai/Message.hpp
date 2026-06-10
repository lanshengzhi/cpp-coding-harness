#pragma once

#include "Content.hpp"
#include "Usage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::ai {

using TimestampMs = std::int64_t;

struct SystemMessage {
    std::string content;
    TimestampMs timestamp{};
};

struct UserMessage {
    std::vector<Content> content;
    TimestampMs timestamp{};
};

struct AssistantMessage {
    std::vector<Content> content;
    std::string api;
    std::string provider;
    std::string model;
    std::optional<std::string> response_model;
    std::optional<std::string> response_id;
    std::optional<Usage> usage;
    AssistantStopReason stop_reason{AssistantStopReason::Unknown};
    std::optional<std::string> error_message;
    TimestampMs timestamp{};
};

struct ToolResultMessage {
    std::string tool_call_id;
    std::string tool_name;
    std::vector<Content> content;
    std::optional<glz::generic> details;
    bool is_error{false};
    TimestampMs timestamp{};
};

using MessageVariant = std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage>;

[[nodiscard]] inline UserMessage user_text_message(std::string text, TimestampMs timestamp = 0) {
    UserMessage message;
    message.content.emplace_back(text_content(std::move(text)));
    message.timestamp = timestamp;
    return message;
}

[[nodiscard]] inline AssistantMessage assistant_text_message(std::string text, TimestampMs timestamp = 0) {
    AssistantMessage message;
    message.content.emplace_back(text_content(std::move(text)));
    message.stop_reason = AssistantStopReason::Stop;
    message.timestamp = timestamp;
    return message;
}

[[nodiscard]] inline ToolResultMessage tool_result_message(
    std::string tool_call_id,
    std::string tool_name,
    std::string text,
    bool is_error = false,
    TimestampMs timestamp = 0) {
    ToolResultMessage message;
    message.tool_call_id = std::move(tool_call_id);
    message.tool_name = std::move(tool_name);
    message.content.emplace_back(text_content(std::move(text)));
    message.is_error = is_error;
    message.timestamp = timestamp;
    return message;
}

} // namespace cch::ai
