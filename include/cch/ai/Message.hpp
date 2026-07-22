#pragma once

#include "Content.hpp"
#include "Usage.hpp"
#include "../util/JsonValue.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::ai {

using TimestampMs = std::int64_t;

struct DiagnosticErrorInfo {
    std::optional<std::string> name;
    std::string message;
    std::optional<std::string> stack;
    std::optional<std::string> code;
};

struct DiagnosticEntry {
    std::string type;
    TimestampMs timestamp{};
    std::optional<DiagnosticErrorInfo> error;
    std::optional<util::JsonValue> details;
};

struct SystemMessage {
    std::string content;
    TimestampMs timestamp{};
};

struct UserMessage {
    std::vector<Content> content;
    TimestampMs timestamp{};
};

struct AssistantMessage {
    std::vector<AssistantContent> content;
    std::string api;
    std::string provider;
    std::string model;
    std::optional<std::string> response_model;
    std::optional<std::string> response_id;
    Usage usage{};
    AssistantStopReason stop_reason{AssistantStopReason::Stop};
    std::optional<std::string> error_message;
    std::optional<std::vector<DiagnosticEntry>> diagnostics;
    TimestampMs timestamp{};
};

struct ToolResultMessage {
    std::string tool_call_id;
    std::string tool_name;
    std::vector<Content> content;
    std::optional<util::JsonValue> details;
    bool is_error{false};
    TimestampMs timestamp{};
};

// ── pi extended runtime message types ──

struct BashExecutionMessage {
    std::string command;
    std::string output;
    std::optional<int> exit_code;
    bool cancelled{false};
    bool truncated{false};
    std::optional<std::string> full_output_path;
    bool exclude_from_context{false};
    TimestampMs timestamp{};
};

struct CustomMessage {
    std::string custom_type;
    std::vector<Content> content;
    bool display{true};
    std::optional<util::JsonValue> details;
    TimestampMs timestamp{};
};

struct BranchSummaryMessage {
    std::string summary;
    std::string from_id;
    TimestampMs timestamp{};
};

struct CompactionSummaryMessage {
    std::string summary;
    std::int64_t tokens_before{0};
    TimestampMs timestamp{};
};

using MessageVariant = std::variant<
    SystemMessage,
    UserMessage,
    AssistantMessage,
    ToolResultMessage,
    BashExecutionMessage,
    CustomMessage,
    BranchSummaryMessage,
    CompactionSummaryMessage>;

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

// ── LLM conversion prefix/suffix constants ──

inline constexpr std::string_view COMPACTION_SUMMARY_PREFIX =
    "The conversation history before this point was compacted into the following summary:\n\n<summary>\n";
inline constexpr std::string_view COMPACTION_SUMMARY_SUFFIX = "\n</summary>";
inline constexpr std::string_view BRANCH_SUMMARY_PREFIX =
    "The following is a summary of a branch that this conversation came back from:\n\n<summary>\n";
inline constexpr std::string_view BRANCH_SUMMARY_SUFFIX = "\n</summary>";

// ── LLM conversion helpers (produce UserMessage from extended types) ──

[[nodiscard]] inline UserMessage bash_execution_to_user_message(const BashExecutionMessage& msg) {
    std::string text;
    text += "Ran `";
    text += msg.command;
    text += "`\n";
    if (!msg.output.empty()) {
        text += "```\n";
        text += msg.output;
        text += "\n```";
    } else {
        text += "(no output)";
    }
    if (msg.cancelled) {
        text += "\n\n(command cancelled)";
    }
    if (msg.exit_code.has_value() && *msg.exit_code != 0) {
        text += "\n\nCommand exited with code ";
        text += std::to_string(*msg.exit_code);
    }
    if (msg.truncated && msg.full_output_path.has_value()) {
        text += "\n\n[Output truncated. Full output: ";
        text += *msg.full_output_path;
        text += "]";
    }
    return user_text_message(std::move(text), msg.timestamp);
}

[[nodiscard]] inline UserMessage branch_summary_to_user_message(const BranchSummaryMessage& msg) {
    std::string text;
    text += BRANCH_SUMMARY_PREFIX;
    text += msg.summary;
    text += BRANCH_SUMMARY_SUFFIX;
    return user_text_message(std::move(text), msg.timestamp);
}

[[nodiscard]] inline UserMessage compaction_summary_to_user_message(const CompactionSummaryMessage& msg) {
    std::string text;
    text += COMPACTION_SUMMARY_PREFIX;
    text += msg.summary;
    text += COMPACTION_SUMMARY_SUFFIX;
    return user_text_message(std::move(text), msg.timestamp);
}

[[nodiscard]] inline UserMessage custom_message_to_user_message(const CustomMessage& msg) {
    if (msg.content.empty()) {
        return user_text_message("", msg.timestamp);
    }
    // Build text from content blocks
    std::string text;
    for (const auto& block : msg.content) {
        auto* tc = std::get_if<TextContent>(&block);
        if (tc) {
            if (!text.empty()) text += "\n";
            text += tc->text;
        }
    }
    return user_text_message(std::move(text), msg.timestamp);
}

} // namespace cch::ai
