#include "durable/ToolContent.hpp"

#include <cch/support/JsonValue.hpp>

#include <format>
#include <type_traits>
#include <utility>
#include <variant>

namespace cch::durable {
namespace {

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

support::Error validation(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

const Json* member(const Json& value, std::string_view key) {
    const auto* object = value.as_object();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

} // namespace

chord::Json content_block_json(const ai::Content& block) {
    return std::visit(
            [](const auto& item) -> Json {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, ai::TextContent>) {
                    Object value{{"type", "text"}, {"text", item.text}};
                    if (item.text_signature) value.emplace("textSignature", *item.text_signature);
                    return Json{std::move(value)};
                } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
                    Object value{{"type", "thinking"}, {"thinking", item.thinking}};
                    if (item.thinking_signature) value.emplace("thinkingSignature", *item.thinking_signature);
                    if (item.redacted) value.emplace("redacted", true);
                    return Json{std::move(value)};
                } else if constexpr (std::is_same_v<T, ai::ImageContent>) {
                    return Json{Object{{"type", "image"}, {"data", item.data}, {"mimeType", item.mime_type}}};
                } else {
                    Object value{{"type", "toolCall"},
                            {"id", item.id},
                            {"name", item.name},
                            {"arguments", item.arguments ? *Json::from_support(*item.arguments) : Json{Object{}}}};
                    if (item.thought_signature) value.emplace("thoughtSignature", *item.thought_signature);
                    return Json{std::move(value)};
                }
            },
            block);
}

chord::Json content_block_json(const ai::AssistantContent& block) {
    return std::visit(
            [](const auto& item) -> Json {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, ai::ToolCallContent>) {
                    Object value{{"type", "toolCall"},
                            {"id", item.id},
                            {"name", item.name},
                            {"arguments", item.arguments ? *Json::from_support(*item.arguments) : Json{Object{}}}};
                    if (item.thought_signature) value.emplace("thoughtSignature", *item.thought_signature);
                    return Json{std::move(value)};
                } else {
                    return content_block_json(ai::Content{item});
                }
            },
            block);
}

chord::Json content_json(std::span<const ai::Content> blocks) {
    Array result;
    for (const auto& block : blocks)
        result.push_back(content_block_json(block));
    return Json{std::move(result)};
}

chord::Json content_json(std::span<const ai::AssistantContent> blocks) {
    Array result;
    for (const auto& block : blocks)
        result.push_back(content_block_json(block));
    return Json{std::move(result)};
}

chord::Json tool_call_json(const ai::ToolCallContent& call, const chord::Json& arguments) {
    Object value{{"type", "toolCall"}, {"id", call.id}, {"name", call.name}, {"arguments", arguments}};
    if (call.thought_signature) value.emplace("thoughtSignature", *call.thought_signature);
    return Json{std::move(value)};
}

support::Expected<std::vector<ai::Content>> content_from_json(const chord::Json& value) {
    const auto* blocks = value.as_array();
    if (blocks == nullptr) return std::unexpected(validation("Hook content must be an array"));
    std::vector<ai::Content> content;
    content.reserve(blocks->size());
    for (const Json& item : *blocks) {
        const Json* type = member(item, "type");
        if (type == nullptr || type->as_string() == nullptr) {
            return std::unexpected(validation("A content block must name its type"));
        }
        if (*type->as_string() == "text") {
            const Json* text = member(item, "text");
            if (text == nullptr || text->as_string() == nullptr) {
                return std::unexpected(validation("A text block must carry text"));
            }
            content.push_back(ai::text_content(*text->as_string()));
        } else if (*type->as_string() == "image") {
            const Json* data = member(item, "data");
            const Json* mime = member(item, "mimeType");
            if (data == nullptr || data->as_string() == nullptr || mime == nullptr || mime->as_string() == nullptr) {
                return std::unexpected(validation("An image block must carry data and mimeType"));
            }
            content.push_back(ai::image_content(*data->as_string(), *mime->as_string()));
        } else if (*type->as_string() == "thinking") {
            const Json* thinking = member(item, "thinking");
            if (thinking == nullptr || thinking->as_string() == nullptr) {
                return std::unexpected(validation("A thinking block must carry thinking"));
            }
            content.push_back(ai::thinking_content(*thinking->as_string()));
        } else {
            return std::unexpected(validation("A tool result block must be text, image or thinking"));
        }
    }
    return content;
}

std::vector<ai::Content> harness_error_content(std::string_view message) {
    return {ai::text_content(std::format("<harness>\n[error] {}\n</harness>", message))};
}

} // namespace cch::durable
