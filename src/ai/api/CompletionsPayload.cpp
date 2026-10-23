#include "CompletionsPayload.hpp"

#include "MessageNormalization.hpp"
#include "MessageText.hpp"
#include "ai/Headers.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

using JsonObject = support::JsonValue::object_t;

/// The wire effort for a requested thinking level: the model's catalog value
/// when it maps the level, the level's own name otherwise. `std::nullopt`
/// means the level has no effort value — off, or a level the catalog marks
/// unsupported — and the caller decides what that means on the wire.
[[nodiscard]] std::optional<std::string> mapped_effort(
        const Model& model, std::optional<ModelThinkingLevel> requested) {
    if (!requested || *requested == ModelThinkingLevel::Off) {
        return std::nullopt;
    }
    if (model.thinking_level_map) {
        if (const auto found = model.thinking_level_map->find(*requested); found != model.thinking_level_map->end()) {
            return found->second;
        }
    }
    const auto name = model_thinking_level_name(*requested);
    return name ? std::optional<std::string>{std::string{*name}} : std::nullopt;
}

[[nodiscard]] support::JsonValue image_part(const ImageContent& image) {
    return JsonObject{
            {"image_url",
                    JsonObject{
                            {"url", "data:" + image.mime_type + ";base64," + image.data},
                    }},
            {"type", "image_url"},
    };
}

[[nodiscard]] support::JsonValue::array_t user_content(const std::vector<Content>& content) {
    support::JsonValue::array_t result;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            if (!text->text.empty()) {
                result.emplace_back(JsonObject{
                        {"text", sanitize_text(text->text)},
                        {"type", "text"},
                });
            }
        } else if (const auto* image = std::get_if<ImageContent>(&block)) {
            result.emplace_back(image_part(*image));
        }
    }
    return result;
}

[[nodiscard]] support::JsonValue tool_result_text(const std::vector<Content>& content) {
    std::string text;
    bool has_image = false;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<TextContent>(&block)) {
            if (!text.empty()) {
                text += '\n';
            }
            text += text_block->text;
        } else if (std::holds_alternative<ImageContent>(block)) {
            has_image = true;
        }
    }
    if (!text.empty()) {
        return sanitize_text(text);
    }
    return has_image ? support::JsonValue{"(see attached image)"} : support::JsonValue{"(no tool output)"};
}

[[nodiscard]] std::optional<support::JsonValue> signature_json(const std::optional<std::string>& signature) {
    if (!signature || signature->empty()) {
        return std::nullopt;
    }
    auto parsed = support::read_json(*signature);
    if (!parsed || (!parsed->holds<support::JsonValue::array_t>() && !parsed->holds<support::JsonValue::object_t>())) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

/// The three reasoning channel spellings a Chat Completions stream may use.
/// A stored thinking signature is the field name the reasoning arrived under,
/// so replay sends it back the same way.
[[nodiscard]] bool is_reasoning_field(std::string_view signature) {
    return signature == "reasoning" || signature == "reasoning_content" || signature == "reasoning_text";
}

[[nodiscard]] support::Expected<std::string> tool_arguments(const ToolCallContent& call) {
    if (call.arguments) {
        return support::write_json(*call.arguments);
    }
    if (!call.raw_arguments.empty()) {
        return call.raw_arguments;
    }
    return std::string{"{}"};
}

[[nodiscard]] support::Expected<JsonObject> assistant_message(const AssistantMessage& assistant) {
    std::string text;
    std::vector<const ThinkingContent*> thinking;
    std::vector<const ToolCallContent*> calls;
    for (const auto& content : assistant.content) {
        if (const auto* text_content = std::get_if<TextContent>(&content)) {
            if (!blank(text_content->text)) {
                text += sanitize_text(text_content->text);
            }
        } else if (const auto* thinking_content = std::get_if<ThinkingContent>(&content)) {
            thinking.push_back(thinking_content);
        } else {
            calls.push_back(&std::get<ToolCallContent>(content));
        }
    }

    JsonObject result{
            {"content", text.empty() ? support::JsonValue{nullptr} : support::JsonValue{std::move(text)}},
            {"role", "assistant"},
    };
    std::optional<std::string> reasoning_field;
    std::optional<support::JsonValue> reasoning_details;
    std::string reasoning_text;
    for (const auto* block : thinking) {
        if (!block->thinking.empty()) {
            if (!reasoning_text.empty()) {
                reasoning_text += '\n';
            }
            reasoning_text += sanitize_text(block->thinking);
        }
        if (!reasoning_details) {
            reasoning_details = signature_json(block->thinking_signature);
        }
        if (!reasoning_field && block->thinking_signature && is_reasoning_field(*block->thinking_signature)) {
            reasoning_field = *block->thinking_signature;
        }
    }
    for (const auto* call : calls) {
        if (!reasoning_details) {
            reasoning_details = signature_json(call->thought_signature);
        }
    }
    if (reasoning_details) {
        result.emplace("reasoning_details", std::move(*reasoning_details));
    } else if (!reasoning_text.empty() && reasoning_field) {
        result.emplace(*reasoning_field, std::move(reasoning_text));
    }

    if (!calls.empty()) {
        support::JsonValue::array_t tool_calls;
        tool_calls.reserve(calls.size());
        for (const auto* call : calls) {
            auto arguments = tool_arguments(*call);
            if (!arguments) {
                return std::unexpected(arguments.error());
            }
            tool_calls.emplace_back(JsonObject{
                    {"function",
                            JsonObject{
                                    {"arguments", std::move(*arguments)},
                                    {"name", call->name},
                            }},
                    {"id", call->id},
                    {"type", "function"},
            });
        }
        result.emplace("tool_calls", std::move(tool_calls));
    }
    return result;
}

[[nodiscard]] bool has_tool_history(const std::vector<MessageVariant>& messages) {
    return std::ranges::any_of(messages, [](const MessageVariant& message) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            return std::ranges::any_of(assistant->content,
                    [](const AssistantContent& content) { return std::holds_alternative<ToolCallContent>(content); });
        }
        return std::holds_alternative<ToolResultMessage>(message);
    });
}

[[nodiscard]] support::Expected<support::JsonValue::array_t> convert_messages(
        const Model& model, const AiContext& context, const std::vector<MessageVariant>& messages) {
    support::JsonValue::array_t result;
    if (context.system_prompt && !context.system_prompt->empty()) {
        result.emplace_back(JsonObject{
                {"content", sanitize_text(*context.system_prompt)},
                {"role", "system"},
        });
    }
    for (const auto& message : messages) {
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            if (const auto* text = std::get_if<std::string>(&user->content)) {
                result.emplace_back(JsonObject{
                        {"content", sanitize_text(*text)},
                        {"role", "user"},
                });
            } else {
                auto content = user_content(std::get<std::vector<Content>>(user->content));
                if (!content.empty()) {
                    result.emplace_back(JsonObject{
                            {"content", std::move(content)},
                            {"role", "user"},
                    });
                }
            }
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            auto converted = assistant_message(*assistant);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            const auto& content = converted->at("content");
            const bool has_content = content.holds<std::string>() && !content.get_string().empty();
            const auto tool_calls = converted->find("tool_calls");
            if (has_content || (tool_calls != converted->end() && !tool_calls->second.get_array().empty())) {
                result.emplace_back(std::move(*converted));
            }
        } else if (const auto* tool_result = std::get_if<ToolResultMessage>(&message)) {
            result.emplace_back(JsonObject{
                    {"content", tool_result_text(tool_result->content)},
                    {"role", "tool"},
                    {"tool_call_id", tool_result->tool_call_id},
            });
            if (supports_images(model) && std::ranges::any_of(tool_result->content, [](const Content& content) {
                    return std::holds_alternative<ImageContent>(content);
                })) {
                support::JsonValue::array_t attached{JsonObject{
                        {"text", "Attached image(s) from tool result:"},
                        {"type", "text"},
                }};
                for (const auto& content : tool_result->content) {
                    if (const auto* image = std::get_if<ImageContent>(&content)) {
                        attached.emplace_back(image_part(*image));
                    }
                }
                result.emplace_back(JsonObject{
                        {"content", std::move(attached)},
                        {"role", "user"},
                });
            }
        }
    }
    return result;
}

[[nodiscard]] support::JsonValue::array_t completions_tools(const std::vector<Tool>& tools) {
    support::JsonValue::array_t result;
    for (const auto& tool : tools) {
        result.emplace_back(JsonObject{
                {"function",
                        JsonObject{
                                {"description", sanitize_text(tool.description)},
                                {"name", tool.name},
                                {"parameters", tool.parameters},
                        }},
                {"type", "function"},
        });
    }
    return result;
}

} // namespace

/// The one Chat Completions request body the Magpie gateway takes (ADR 0074):
/// one field set for every provider id. Thinking is stated as
/// `reasoning_effort` — including the explicit `"none"` a thinking-off request
/// sends — and the session identity rides `X-Magpie-Session` and
/// `prompt_cache_key`.
support::Expected<support::JsonValue> build_completions_payload(
        const Model& model, const AiContext& context, const PreparedStreamOptions& options) {
    if (auto valid = validate_model(model); !valid) {
        return std::unexpected(valid.error());
    }
    const auto normalized = normalize_history(model, context);
    auto messages = convert_messages(model, context, normalized);
    if (!messages) {
        return std::unexpected(messages.error());
    }

    support::JsonValue::array_t tools;
    if (!context.tools.empty()) {
        tools = completions_tools(context.tools);
    }

    support::JsonValue::object_t payload{
            {"messages", std::move(*messages)},
            {"model", model.id},
            {"stream", true},
    };
    payload.emplace("stream_options", JsonObject{{"include_usage", true}});
    if (options.max_tokens > 0) {
        payload.emplace("max_completion_tokens", static_cast<double>(options.max_tokens));
    }
    if (options.temperature) {
        payload.emplace("temperature", *options.temperature);
    }
    if (!tools.empty() || has_tool_history(normalized)) {
        payload.emplace("tools", std::move(tools));
    }
    if (model.reasoning) {
        const auto effort = mapped_effort(model, options.reasoning);
        if (effort) {
            payload.emplace("reasoning_effort", *effort);
        } else if (!options.reasoning || *options.reasoning == ModelThinkingLevel::Off) {
            // The gateway's explicit thinking-off value: it disables or
            // minimizes reasoning and falls back to the nearest level when the
            // model cannot stop thinking.
            payload.emplace("reasoning_effort", "none");
        }
    }
    if (options.cache_retention != CacheRetention::None && options.session_id) {
        payload.emplace("prompt_cache_key", detail::clamp_prompt_cache_key(*options.session_id));
    }
    return support::JsonValue{std::move(payload)};
}

} // namespace cch::ai::api
