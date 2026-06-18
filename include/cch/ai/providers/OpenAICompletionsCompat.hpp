#pragma once

#include <optional>
#include <string>

namespace cch::ai::providers {

enum class ThinkingFormat {
    openai,
    openrouter,
    deepseek,
    together,
    zai,
    qwen,
    qwen_chat_template,
    string_thinking,
    ant_ling,
};

enum class MaxTokensField {
    max_tokens,
    max_completion_tokens,
};

struct OpenAICompletionsCompat {
    std::optional<bool> supports_store;
    std::optional<bool> supports_developer_role;
    std::optional<bool> supports_reasoning_effort;
    std::optional<bool> supports_usage_in_streaming;
    std::optional<MaxTokensField> max_tokens_field;
    std::optional<bool> requires_tool_result_name;
    std::optional<bool> requires_assistant_after_tool_result;
    std::optional<bool> requires_thinking_as_text;
    std::optional<bool> requires_reasoning_content_on_assistant_messages;
    std::optional<ThinkingFormat> thinking_format;
};

} // namespace cch::ai::providers
