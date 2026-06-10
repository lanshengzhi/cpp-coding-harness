#pragma once

#include "../util/JsonValue.hpp"

#include <optional>
#include <string>
#include <variant>

namespace cch::ai {

struct TextContent {
    std::string text;
    std::optional<std::string> text_signature;
};

struct ThinkingContent {
    std::string thinking;
    std::optional<std::string> thinking_signature;
    bool redacted{false};
};

struct ImageContent {
    std::string data;
    std::string mime_type;
};

struct ToolCallContent {
    std::string id;
    std::string name;
    std::optional<util::JsonValue> arguments;
    std::string raw_arguments;
    std::optional<std::string> thought_signature;
    bool arguments_valid{true};
    std::optional<std::string> argument_error;
};

using Content = std::variant<TextContent, ThinkingContent, ImageContent, ToolCallContent>;

[[nodiscard]] inline TextContent text_content(std::string text) {
    return TextContent{std::move(text), std::nullopt};
}

[[nodiscard]] inline ThinkingContent thinking_content(std::string thinking) {
    return ThinkingContent{std::move(thinking), std::nullopt, false};
}

[[nodiscard]] inline ImageContent image_content(std::string data, std::string mime_type) {
    return ImageContent{std::move(data), std::move(mime_type)};
}

[[nodiscard]] inline ToolCallContent tool_call_content(
    std::string id,
    std::string name,
    std::string raw_arguments,
    std::optional<util::JsonValue> arguments = std::nullopt) {
    return ToolCallContent{
        std::move(id),
        std::move(name),
        std::move(arguments),
        std::move(raw_arguments),
        std::nullopt,
        true,
        std::nullopt,
    };
}

} // namespace cch::ai
