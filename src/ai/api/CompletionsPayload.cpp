#include "PayloadBuilders.hpp"

#include "MessageNormalization.hpp"
#include "MessageText.hpp"
#include "ProviderDetection.hpp"
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

struct ResolvedCompletionsCompat {
    bool supports_store{false};
    bool supports_developer_role{false};
    bool supports_strict_mode{false};
    bool supports_reasoning_effort{true};
    bool requires_reasoning_content{false};
    bool supports_long_cache_retention{true};
    OpenAICompletionsMaxTokensField max_tokens_field{OpenAICompletionsMaxTokensField::MaxCompletionTokens};
    OpenAICompletionsThinkingFormat thinking_format{OpenAICompletionsThinkingFormat::OpenAI};
    std::optional<OpenAICompletionsCacheControlFormat> cache_control_format{std::nullopt};
};

[[nodiscard]] const OpenAICompletionsCompat* completions_compat(const Model& model) {
    if (!model.compat) {
        return nullptr;
    }
    return std::get_if<OpenAICompletionsCompat>(&*model.compat);
}

[[nodiscard]] ResolvedCompletionsCompat resolve_compat(const Model& model) {
    const bool deepseek = is_deepseek(model);
    const bool openrouter = is_openrouter(model);
    const bool openrouter_developer_model =
            openrouter && (model.id.starts_with("openai/") || model.id.starts_with("anthropic/"));

    ResolvedCompletionsCompat resolved{
            .supports_store = !deepseek,
            .supports_developer_role = openrouter_developer_model || (!openrouter && !deepseek),
            .supports_strict_mode = false,
            .supports_reasoning_effort = true,
            .requires_reasoning_content = deepseek,
            .supports_long_cache_retention = true,
            .max_tokens_field = deepseek ? OpenAICompletionsMaxTokensField::MaxTokens
                                         : OpenAICompletionsMaxTokensField::MaxCompletionTokens,
            .thinking_format = deepseek     ? OpenAICompletionsThinkingFormat::DeepSeek
                               : openrouter ? OpenAICompletionsThinkingFormat::OpenRouter
                                            : OpenAICompletionsThinkingFormat::OpenAI,
            .cache_control_format =
                    openrouter && model.id.starts_with("anthropic/")
                            ? std::optional<OpenAICompletionsCacheControlFormat>{OpenAICompletionsCacheControlFormat::
                                              Anthropic}
                            : std::nullopt,
    };
    if (const auto* compat = completions_compat(model)) {
        if (compat->supports_store) {
            resolved.supports_store = *compat->supports_store;
        }
        if (compat->supports_developer_role) {
            resolved.supports_developer_role = *compat->supports_developer_role;
        }
        if (compat->supports_strict_mode) {
            resolved.supports_strict_mode = *compat->supports_strict_mode;
        }
        if (compat->max_tokens_field) {
            resolved.max_tokens_field = *compat->max_tokens_field;
        }
        if (compat->requires_reasoning_content_on_assistant_messages) {
            resolved.requires_reasoning_content = *compat->requires_reasoning_content_on_assistant_messages;
        }
        if (compat->thinking_format) {
            resolved.thinking_format = *compat->thinking_format;
        }
        if (compat->cache_control_format) {
            resolved.cache_control_format = *compat->cache_control_format;
        }
        if (compat->supports_long_cache_retention) {
            resolved.supports_long_cache_retention = *compat->supports_long_cache_retention;
        }
        if (compat->supports_reasoning_effort) {
            resolved.supports_reasoning_effort = *compat->supports_reasoning_effort;
        }
    }
    return resolved;
}

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

[[nodiscard]] std::optional<std::string> mapped_off(const Model& model) {
    if (!model.thinking_level_map) {
        return std::nullopt;
    }
    const auto found = model.thinking_level_map->find(ModelThinkingLevel::Off);
    if (found == model.thinking_level_map->end()) {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] bool supports_reasoning_off(const Model& model) {
    if (!model.thinking_level_map) {
        return true;
    }
    const auto found = model.thinking_level_map->find(ModelThinkingLevel::Off);
    return found == model.thinking_level_map->end() || found->second.has_value();
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

[[nodiscard]] std::string normalize_completions_id(const Model& model, std::string_view value) {
    const auto separator = value.find('|');
    if (separator == std::string_view::npos) {
        if (model.provider == "openai" && value.size() > 40) {
            return std::string{value.substr(0, 40)};
        }
        return std::string{value};
    }
    const auto call_id = normalize_id_part(value.substr(0, separator), true);
    const auto item_id = normalize_id_part(value.substr(separator + 1), true);
    auto combined = item_id.empty() ? call_id : call_id + "_" + item_id;
    if (combined.size() <= 40) {
        return combined;
    }
    const auto hash = short_hash(value).substr(0, 8);
    const auto prefix_size = std::max<std::size_t>(1, 40 - hash.size() - 1);
    return call_id.substr(0, std::min(prefix_size, call_id.size())) + "_" + hash;
}

[[nodiscard]] support::Expected<JsonObject> assistant_message(
        const AssistantMessage& assistant, const Model& model, const ResolvedCompletionsCompat& compat) {
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
    if (compat.requires_reasoning_content && model.reasoning && !result.contains("reasoning_content")) {
        result.emplace("reasoning_content", "");
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
                    {"id", normalize_completions_id(model, call->id)},
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

[[nodiscard]] bool add_cache_control_to_content(JsonObject& message, const support::JsonValue& cache_control) {
    auto found = message.find("content");
    if (found == message.end()) {
        return false;
    }
    if (auto* text = found->second.get_if<std::string>()) {
        if (text->empty()) {
            return false;
        }
        found->second = support::JsonValue::array_t{JsonObject{
                {"cache_control", cache_control},
                {"text", std::move(*text)},
                {"type", "text"},
        }};
        return true;
    }
    auto* blocks = found->second.get_if<support::JsonValue::array_t>();
    if (!blocks) {
        return false;
    }
    for (auto it = blocks->rbegin(); it != blocks->rend(); ++it) {
        auto* block = it->get_if<JsonObject>();
        if (!block) {
            continue;
        }
        const auto type = block->find("type");
        if (type != block->end() && type->second.holds<std::string>() && type->second.get_string() == "text") {
            block->emplace("cache_control", cache_control);
            return true;
        }
    }
    return false;
}

void apply_cache_control(support::JsonValue::array_t& messages,
        support::JsonValue::array_t& tools,
        CacheRetention retention,
        bool long_retention) {
    if (retention == CacheRetention::None) {
        return;
    }
    JsonObject cache_object{{"type", "ephemeral"}};
    if (retention == CacheRetention::Long && long_retention) {
        cache_object.emplace("ttl", "1h");
    }
    const support::JsonValue cache_control{cache_object};
    for (auto& message : messages) {
        auto* object = message.get_if<JsonObject>();
        if (!object) {
            continue;
        }
        const auto role = object->find("role");
        if (role != object->end() && role->second.holds<std::string>() &&
                (role->second.get_string() == "system" || role->second.get_string() == "developer")) {
            static_cast<void>(add_cache_control_to_content(*object, cache_control));
            break;
        }
    }
    if (!tools.empty()) {
        tools.back().get_object().emplace("cache_control", cache_control);
    }
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        auto* object = it->get_if<JsonObject>();
        if (!object) {
            continue;
        }
        const auto role = object->find("role");
        if (role != object->end() && role->second.holds<std::string>() &&
                (role->second.get_string() == "user" || role->second.get_string() == "assistant" ||
                        role->second.get_string() == "tool") &&
                add_cache_control_to_content(*object, cache_control)) {
            break;
        }
    }
}

[[nodiscard]] support::Expected<support::JsonValue::array_t> convert_messages(const Model& model,
        const AiContext& context,
        const std::vector<MessageVariant>& messages,
        const ResolvedCompletionsCompat& compat) {
    support::JsonValue::array_t result;
    if (context.system_prompt && !context.system_prompt->empty()) {
        result.emplace_back(JsonObject{
                {"content", sanitize_text(*context.system_prompt)},
                {"role", model.reasoning && compat.supports_developer_role ? "developer" : "system"},
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
            auto converted = assistant_message(*assistant, model, compat);
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
                    {"tool_call_id", normalize_completions_id(model, tool_result->tool_call_id)},
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

[[nodiscard]] support::JsonValue::array_t completions_tools(
        const ResolvedCompletionsCompat& compat, const std::vector<Tool>& tools) {
    support::JsonValue::array_t result;
    for (const auto& tool : tools) {
        JsonObject function{
                {"description", sanitize_text(tool.description)},
                {"name", tool.name},
                {"parameters", tool.parameters},
        };
        if (compat.supports_strict_mode) {
            function.emplace("strict", false);
        }
        result.emplace_back(JsonObject{
                {"function", std::move(function)},
                {"type", "function"},
        });
    }
    return result;
}

[[nodiscard]] std::optional<std::string> reasoning_off(const Model& model, const ResolvedCompletionsCompat& compat) {
    if (!model.reasoning || !compat.supports_reasoning_effort) {
        return std::nullopt;
    }
    return mapped_off(model);
}

} // namespace

support::Expected<support::JsonValue> build_completions_payload(
        const Model& model, const AiContext& context, const ProviderStreamOptions& options) {
    const auto compat = resolve_compat(model);
    const auto normalized = normalize_history(AdapterKind::OpenAICompletions, model, context);
    auto messages = convert_messages(model, context, normalized, compat);
    if (!messages) {
        return std::unexpected(messages.error());
    }

    support::JsonValue::array_t tools;
    if (!context.tools.empty()) {
        tools = completions_tools(compat, context.tools);
    } else {
        if (has_tool_history(normalized)) {
            tools = {};
        }
    }
    if (compat.cache_control_format && *compat.cache_control_format == OpenAICompletionsCacheControlFormat::Anthropic) {
        apply_cache_control(*messages, tools, options.cache_retention, compat.supports_long_cache_retention);
    }

    support::JsonValue::object_t payload{
            {"messages", std::move(*messages)},
            {"model", model.id},
            {"stream", true},
    };
    payload.emplace("stream_options", JsonObject{{"include_usage", true}});
    if (compat.supports_store) {
        payload.emplace("store", false);
    }
    if (options.max_tokens > 0) {
        if (compat.max_tokens_field == OpenAICompletionsMaxTokensField::MaxTokens) {
            payload.emplace("max_tokens", static_cast<double>(options.max_tokens));
        } else {
            payload.emplace("max_completion_tokens", static_cast<double>(options.max_tokens));
        }
    }
    if (options.temperature) {
        payload.emplace("temperature", *options.temperature);
    }
    if (!tools.empty() || has_tool_history(normalized)) {
        payload.emplace("tools", std::move(tools));
    }

    const auto effort = mapped_effort(model, options.reasoning);
    const auto off = reasoning_off(model, compat);
    switch (compat.thinking_format) {
    case OpenAICompletionsThinkingFormat::DeepSeek: {
        if (model.reasoning) {
            if (effort) {
                payload.emplace("thinking", JsonObject{{"type", "enabled"}});
            } else if (supports_reasoning_off(model)) {
                payload.emplace("thinking", JsonObject{{"type", "disabled"}});
            }
            if (effort && compat.supports_reasoning_effort) {
                payload.emplace("reasoning_effort", *effort);
            }
        }
        break;
    }
    case OpenAICompletionsThinkingFormat::OpenRouter: {
        if (model.reasoning && (effort || supports_reasoning_off(model))) {
            payload.emplace("reasoning", JsonObject{{"effort", effort.value_or(off.value_or("none"))}});
        }
        break;
    }
    case OpenAICompletionsThinkingFormat::Qwen: {
        if (model.reasoning) {
            payload.emplace("enable_thinking", effort.has_value());
            if (effort && compat.supports_reasoning_effort) {
                payload.emplace("reasoning_effort", *effort);
            }
        }
        break;
    }
    case OpenAICompletionsThinkingFormat::OpenAI: {
        if (model.reasoning && compat.supports_reasoning_effort) {
            if (effort) {
                payload.emplace("reasoning_effort", *effort);
            } else if (off) {
                payload.emplace("reasoning_effort", *off);
            }
        }
        break;
    }
    }
    if (options.cache_retention == CacheRetention::Long && compat.supports_long_cache_retention) {
        payload.emplace("prompt_cache_retention", "24h");
        if (options.session_id) {
            payload.emplace("prompt_cache_key", detail::clamp_openai_prompt_cache_key(*options.session_id));
        }
    } else if (contains_case_insensitive(model.base_url, "api.openai.com") &&
               options.cache_retention != CacheRetention::None && options.session_id) {
        payload.emplace("prompt_cache_key", detail::clamp_openai_prompt_cache_key(*options.session_id));
    }
    return support::JsonValue{std::move(payload)};
}

} // namespace cch::ai::api
