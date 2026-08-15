#pragma once

#include <cch/support/JsonValue.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::ai {

struct TextContent {
    std::string text{};
    std::optional<std::string> text_signature{};
};

struct ThinkingContent {
    std::string thinking{};
    std::optional<std::string> thinking_signature{};
    bool redacted{false};
};

struct ImageContent {
    std::string data{};
    std::string mime_type{};
};

struct ToolCallContent {
    std::string id{};
    std::string name{};
    std::optional<cch::support::JsonValue> arguments{};
    std::string raw_arguments{};
    std::optional<std::string> thought_signature{};
    bool arguments_valid{true};
    std::optional<std::string> argument_error{};
};

// §3.3 names variant aliases `*Variant`; `Content` and `AssistantContent`
// predate the rule and keep their short names to avoid public API churn (debt
// recorded in #372).
using Content = std::variant<TextContent, ThinkingContent, ImageContent>;

using AssistantContent = std::variant<TextContent, ThinkingContent, ToolCallContent>;

[[nodiscard]] inline TextContent text_content(std::string text) {
    return TextContent{.text = std::move(text), .text_signature = std::nullopt};
}

[[nodiscard]] inline ThinkingContent thinking_content(std::string thinking) {
    return ThinkingContent{
        .thinking = std::move(thinking),
        .thinking_signature = std::nullopt,
        .redacted = false,
    };
}

[[nodiscard]] inline ImageContent image_content(std::string data, std::string mime_type) {
    return ImageContent{.data = std::move(data), .mime_type = std::move(mime_type)};
}

[[nodiscard]] inline ToolCallContent tool_call_content(
    std::string id,
    std::string name,
    std::string raw_arguments,
    std::optional<cch::support::JsonValue> arguments = std::nullopt) {
    return ToolCallContent{
        .id = std::move(id),
        .name = std::move(name),
        .arguments = std::move(arguments),
        .raw_arguments = std::move(raw_arguments),
        .thought_signature = std::nullopt,
        .arguments_valid = true,
        .argument_error = std::nullopt,
    };
}

/// Presentation/read-only extraction of the concatenated text across a block
/// list. The generic block-walking machinery is implemented in
/// `src/ai/ContentUtil.cpp` so it stays out of the public surface
/// (`docs/agents/architecture.md` §Local generic machinery).
[[nodiscard]] std::string text_from_content(const std::vector<Content>& content);

[[nodiscard]] std::string text_from_assistant_content(
    const std::vector<AssistantContent>& content);

} // namespace cch::ai
