#include "PayloadBuilders.hpp"

#include "MessageNormalization.hpp"
#include "MessageText.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

[[nodiscard]] bool anthropic_supports_temperature(const Model& model) {
    if (!model.compat) {
        return true;
    }
    const auto* compat = std::get_if<AnthropicMessagesCompat>(&*model.compat);
    return compat == nullptr || compat->supports_temperature.value_or(true);
}

[[nodiscard]] bool anthropic_allows_empty_signature(const Model& model) {
    if (!model.compat) {
        return false;
    }
    const auto* compat = std::get_if<AnthropicMessagesCompat>(&*model.compat);
    return compat != nullptr && compat->allow_empty_signature.value_or(false);
}

[[nodiscard]] support::JsonValue anthropic_image(const ImageContent& image) {
    return support::JsonValue::object_t{
            {"source",
                    support::JsonValue::object_t{
                            {"data", image.data},
                            {"media_type", image.mime_type},
                            {"type", "base64"},
                    }},
            {"type", "image"},
    };
}

[[nodiscard]] support::JsonValue::array_t anthropic_user_content(const std::vector<Content>& content) {
    support::JsonValue::array_t result;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            if (!blank(text->text)) {
                result.emplace_back(support::JsonValue::object_t{
                        {"text", sanitize_text(text->text)},
                        {"type", "text"},
                });
            }
        } else if (const auto* image = std::get_if<ImageContent>(&block)) {
            result.emplace_back(anthropic_image(*image));
        }
    }
    return result;
}

[[nodiscard]] support::JsonValue anthropic_tool_result_content(const std::vector<Content>& content) {
    const bool has_images = std::ranges::any_of(
            content, [](const Content& block) { return std::holds_alternative<ImageContent>(block); });
    if (!has_images) {
        std::string text;
        for (const auto& block : content) {
            if (const auto* text_block = std::get_if<TextContent>(&block)) {
                if (!text.empty()) {
                    text += '\n';
                }
                text += text_block->text;
            }
        }
        return sanitize_text(text);
    }
    support::JsonValue::array_t blocks;
    bool has_text = false;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            has_text = true;
            blocks.emplace_back(support::JsonValue::object_t{
                    {"text", sanitize_text(text->text)},
                    {"type", "text"},
            });
        } else if (const auto* image = std::get_if<ImageContent>(&block)) {
            blocks.emplace_back(anthropic_image(*image));
        }
    }
    if (!has_text) {
        blocks.insert(blocks.begin(), support::JsonValue::object_t{{"text", "(see attached image)"}, {"type", "text"}});
    }
    return blocks;
}

[[nodiscard]] support::Expected<support::JsonValue::array_t> convert_anthropic_messages(
        const Model& model, const AiContext& context) {
    const auto messages = normalize_history(AdapterKind::AnthropicMessages, model, context);
    support::JsonValue::array_t result;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (const auto* user = std::get_if<UserMessage>(&messages[index])) {
            if (const auto* text = std::get_if<std::string>(&user->content)) {
                // pi `anthropic-messages.ts`: a non-blank string alternative
                // is sent as a raw sanitized JSON string; blank strings drop
                // the message entirely.
                if (!blank(*text)) {
                    result.emplace_back(support::JsonValue::object_t{
                            {"content", sanitize_text(*text)},
                            {"role", "user"},
                    });
                }
            } else {
                auto content = anthropic_user_content(std::get<std::vector<Content>>(user->content));
                if (!content.empty()) {
                    result.emplace_back(support::JsonValue::object_t{
                            {"content", std::move(content)},
                            {"role", "user"},
                    });
                }
            }
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&messages[index])) {
            support::JsonValue::array_t content;
            for (const auto& block : assistant->content) {
                if (const auto* text = std::get_if<TextContent>(&block)) {
                    if (!blank(text->text)) {
                        content.emplace_back(support::JsonValue::object_t{
                                {"text", sanitize_text(text->text)},
                                {"type", "text"},
                        });
                    }
                } else if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                    if (thinking->redacted) {
                        content.emplace_back(support::JsonValue::object_t{
                                {"data", thinking->thinking_signature.value_or("")},
                                {"type", "redacted_thinking"},
                        });
                    } else {
                        const bool has_signature =
                                thinking->thinking_signature && !blank(*thinking->thinking_signature);
                        if (blank(thinking->thinking) && !has_signature) {
                            continue;
                        }
                        const bool allow_empty = anthropic_allows_empty_signature(model);
                        if (!has_signature && !allow_empty) {
                            content.emplace_back(support::JsonValue::object_t{
                                    {"text", sanitize_text(thinking->thinking)},
                                    {"type", "text"},
                            });
                        } else {
                            content.emplace_back(support::JsonValue::object_t{
                                    {"signature", has_signature ? *thinking->thinking_signature : ""},
                                    {"thinking", sanitize_text(thinking->thinking)},
                                    {"type", "thinking"},
                            });
                        }
                    }
                } else if (const auto* call = std::get_if<ToolCallContent>(&block)) {
                    content.emplace_back(support::JsonValue::object_t{
                            {"id", call->id},
                            {"input", call->arguments.value_or(support::JsonValue::object_t{})},
                            {"name", call->name},
                            {"type", "tool_use"},
                    });
                }
            }
            if (!content.empty()) {
                result.emplace_back(support::JsonValue::object_t{
                        {"content", std::move(content)},
                        {"role", "assistant"},
                });
            }
        } else if (std::holds_alternative<ToolResultMessage>(messages[index])) {
            support::JsonValue::array_t content;
            std::size_t result_index = index;
            while (result_index < messages.size()) {
                const auto* tool_result = std::get_if<ToolResultMessage>(&messages[result_index]);
                if (!tool_result) {
                    break;
                }
                content.emplace_back(support::JsonValue::object_t{
                        {"content", anthropic_tool_result_content(tool_result->content)},
                        {"is_error", tool_result->is_error},
                        {"tool_use_id", tool_result->tool_call_id},
                        {"type", "tool_result"},
                });
                ++result_index;
            }
            index = result_index - 1;
            result.emplace_back(support::JsonValue::object_t{
                    {"content", std::move(content)},
                    {"role", "user"},
            });
        }
    }
    return result;
}

[[nodiscard]] support::JsonValue cache_control(CacheRetention retention) {
    support::JsonValue::object_t result{{"type", "ephemeral"}};
    if (retention == CacheRetention::Long) {
        result.emplace("ttl", "1h");
    }
    return result;
}

void attach_cache_control_to_last_user(support::JsonValue::array_t& messages, CacheRetention retention) {
    if (retention == CacheRetention::None || messages.empty()) {
        return;
    }
    auto* object = messages.back().get_if<support::JsonValue::object_t>();
    if (!object || object->at("role").get_string() != "user") {
        return;
    }
    auto& content = object->at("content");
    if (auto* blocks = content.get_if<support::JsonValue::array_t>()) {
        if (!blocks->empty()) {
            blocks->back().get_object().insert_or_assign("cache_control", cache_control(retention));
        }
    } else if (auto* text = content.get_if<std::string>()) {
        // pi `anthropic-messages.ts`: a trailing string user param is promoted
        // to a one-element cache-marked block array under cache retention.
        content = support::JsonValue{support::JsonValue::array_t{support::JsonValue::object_t{
                {"cache_control", cache_control(retention)},
                {"text", std::move(*text)},
                {"type", "text"},
        }}};
    }
}

[[nodiscard]] support::JsonValue::array_t anthropic_tools(const std::vector<Tool>& tools, CacheRetention retention) {
    support::JsonValue::array_t result;
    for (std::size_t index = 0; index < tools.size(); ++index) {
        const auto& tool = tools[index];
        support::JsonValue::object_t schema{{"type", "object"}};
        if (const auto* parameters = tool.parameters.get_if<support::JsonValue::object_t>()) {
            if (const auto properties = parameters->find("properties"); properties != parameters->end()) {
                schema.emplace("properties", properties->second);
            } else {
                schema.emplace("properties", support::JsonValue::object_t{});
            }
            if (const auto required = parameters->find("required"); required != parameters->end()) {
                schema.emplace("required", required->second);
            } else {
                schema.emplace("required", support::JsonValue::array_t{});
            }
        }
        support::JsonValue::object_t converted{
                {"description", sanitize_text(tool.description)},
                {"eager_input_streaming", true},
                {"input_schema", std::move(schema)},
                {"name", tool.name},
        };
        if (retention != CacheRetention::None && index + 1 == tools.size()) {
            converted.emplace("cache_control", cache_control(retention));
        }
        result.emplace_back(std::move(converted));
    }
    return result;
}

[[nodiscard]] std::string anthropic_effort(const Model& model, ModelThinkingLevel level) {
    if (model.thinking_level_map) {
        if (const auto found = model.thinking_level_map->find(level);
                found != model.thinking_level_map->end() && found->second) {
            return *found->second;
        }
    }
    switch (level) {
    case ModelThinkingLevel::Minimal:
    case ModelThinkingLevel::Low:
        return "low";
    case ModelThinkingLevel::Medium:
        return "medium";
    default:
        return "high";
    }
}

constexpr std::uint64_t kAnthropicMinimumAnswerTokens = 1024;

[[nodiscard]] std::uint64_t default_anthropic_thinking_budget(ModelThinkingLevel level) {
    switch (level) {
    case ModelThinkingLevel::Minimal:
        return 1024;
    case ModelThinkingLevel::Low:
        return 2048;
    case ModelThinkingLevel::Medium:
        return 8192;
    case ModelThinkingLevel::High:
    case ModelThinkingLevel::XHigh:
    case ModelThinkingLevel::Max:
        return 16384;
    case ModelThinkingLevel::Off:
        return 1024;
    }
    return 1024;
}

/// Keep budget-based thinking inside the already-clamped response ceiling.
/// The fallback mirrors pi's Anthropic parameter builder for ceilings that
/// cannot leave the usual 1024-token answer room.
[[nodiscard]] std::uint64_t clamped_anthropic_thinking_budget(ModelThinkingLevel level, std::uint64_t max_tokens) {
    const auto default_budget = default_anthropic_thinking_budget(level);
    if (max_tokens <= kAnthropicMinimumAnswerTokens) {
        return kAnthropicMinimumAnswerTokens;
    }
    return std::min(default_budget, max_tokens - kAnthropicMinimumAnswerTokens);
}

} // namespace

[[nodiscard]] support::Expected<support::JsonValue> build_anthropic_payload(
        const Model& model, const AiContext& context, const ProviderStreamOptions& options) {
    auto messages = convert_anthropic_messages(model, context);
    if (!messages) {
        return std::unexpected(messages.error());
    }
    attach_cache_control_to_last_user(*messages, options.cache_retention);
    support::JsonValue::object_t payload{
            {"max_tokens", static_cast<double>(options.max_tokens)},
            {"messages", std::move(*messages)},
            {"model", model.id},
            {"stream", true},
    };
    if (context.system_prompt && !context.system_prompt->empty()) {
        support::JsonValue::object_t system_block{
                {"text", sanitize_text(*context.system_prompt)},
                {"type", "text"},
        };
        if (options.cache_retention != CacheRetention::None) {
            system_block.emplace("cache_control", cache_control(options.cache_retention));
        }
        payload.emplace("system", support::JsonValue::array_t{std::move(system_block)});
    }
    bool thinking_enabled = false;
    const auto* compat = model.compat ? std::get_if<AnthropicMessagesCompat>(&*model.compat) : nullptr;
    if (model.reasoning && !options.reasoning) {
        if (reasoning_off_supported(model)) {
            payload.emplace("thinking", support::JsonValue::object_t{{"type", "disabled"}});
        }
    } else if (model.reasoning && options.reasoning) {
        const auto level = clamp_thinking_level(model, *options.reasoning);
        if (level == ModelThinkingLevel::Off) {
            if (reasoning_off_supported(model)) {
                payload.emplace("thinking", support::JsonValue::object_t{{"type", "disabled"}});
            }
        } else if (compat && compat->force_adaptive_thinking.value_or(false)) {
            thinking_enabled = true;
            payload.emplace("thinking",
                    support::JsonValue::object_t{
                            {"display", "summarized"},
                            {"type", "adaptive"},
                    });
            payload.emplace("output_config", support::JsonValue::object_t{{"effort", anthropic_effort(model, level)}});
        } else {
            thinking_enabled = true;
            payload.emplace("thinking",
                    support::JsonValue::object_t{
                            {"budget_tokens",
                                    static_cast<double>(clamped_anthropic_thinking_budget(level, options.max_tokens))},
                            {"type", "enabled"},
                    });
        }
    }
    if (options.temperature && !thinking_enabled && anthropic_supports_temperature(model)) {
        payload.emplace("temperature", *options.temperature);
    }
    if (!context.tools.empty()) {
        payload.emplace("tools", anthropic_tools(context.tools, options.cache_retention));
    }
    return support::JsonValue{std::move(payload)};
}

} // namespace cch::ai::api
