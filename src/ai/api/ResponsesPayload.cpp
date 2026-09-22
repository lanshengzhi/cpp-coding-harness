#include "PayloadBuilders.hpp"

#include "MessageNormalization.hpp"
#include "MessageText.hpp"
#include "ai/SimpleOptions.hpp"
#include "support/Json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

[[nodiscard]] support::JsonValue response_image(const ImageContent& image) {
    return support::JsonValue::object_t{
            {"detail", "auto"},
            {"image_url", "data:" + image.mime_type + ";base64," + image.data},
            {"type", "input_image"},
    };
}

[[nodiscard]] support::JsonValue::array_t response_content(const std::vector<Content>& content) {
    support::JsonValue::array_t result;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextContent>(&block)) {
            result.emplace_back(support::JsonValue::object_t{
                    {"text", sanitize_text(text->text)},
                    {"type", "input_text"},
            });
        } else if (const auto* image = std::get_if<ImageContent>(&block)) {
            result.emplace_back(response_image(*image));
        }
    }
    return result;
}

[[nodiscard]] support::JsonValue response_tool_output(const Model& model, const std::vector<Content>& content) {
    std::string text;
    std::vector<ImageContent> images;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<TextContent>(&block)) {
            if (!text.empty()) {
                text += '\n';
            }
            text += text_block->text;
        } else if (const auto* image = std::get_if<ImageContent>(&block)) {
            images.push_back(*image);
        }
    }
    if (images.empty() || !supports_images(model)) {
        if (!text.empty()) {
            return sanitize_text(text);
        }
        return images.empty() ? "(no tool output)" : "(see attached image)";
    }
    support::JsonValue::array_t output;
    if (!text.empty()) {
        output.emplace_back(support::JsonValue::object_t{
                {"text", sanitize_text(text)},
                {"type", "input_text"},
        });
    }
    for (const auto& image : images) {
        output.emplace_back(response_image(image));
    }
    return output;
}

struct ParsedTextSignature {
    std::string id;
    std::optional<std::string> phase;
};

[[nodiscard]] std::optional<ParsedTextSignature> parse_text_signature(const std::optional<std::string>& signature) {
    if (!signature || signature->empty()) {
        return std::nullopt;
    }
    if (signature->starts_with('{')) {
        const auto parsed = support::read_json(*signature);
        if (parsed) {
            const auto* object = parsed->get_if<support::JsonValue::object_t>();
            if (object) {
                const auto version = object->find("v");
                const auto id = object->find("id");
                if (version != object->end() && id != object->end() && version->second.holds<double>() &&
                        version->second.get_number() == 1 && id->second.holds<std::string>()) {
                    ParsedTextSignature result{
                            .id = id->second.get_string(),
                            .phase = std::nullopt,
                    };
                    if (const auto phase = object->find("phase");
                            phase != object->end() && phase->second.holds<std::string>() &&
                            (phase->second.get_string() == "commentary" ||
                                    phase->second.get_string() == "final_answer")) {
                        result.phase = phase->second.get_string();
                    }
                    return result;
                }
            }
        }
    }
    return ParsedTextSignature{.id = *signature, .phase = std::nullopt};
}

[[nodiscard]] support::Expected<support::JsonValue::array_t> convert_responses_messages(
        AdapterKind adapter, const Model& model, const AiContext& context) {
    support::JsonValue::array_t result;
    if (adapter == AdapterKind::OpenAIResponses && context.system_prompt && !context.system_prompt->empty()) {
        result.emplace_back(support::JsonValue::object_t{
                {"content", sanitize_text(*context.system_prompt)},
                {"role", model.reasoning ? "developer" : "system"},
        });
    }

    const auto messages = normalize_history(adapter, model, context);
    std::size_t message_index = 0;
    for (const auto& message : messages) {
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            if (const auto* text = std::get_if<std::string>(&user->content)) {
                // pi `openai-responses-shared.ts`: a string alternative emits
                // exactly one sanitized input_text item, unconditionally
                // (empty string included).
                result.emplace_back(support::JsonValue::object_t{
                        {"content",
                                support::JsonValue::array_t{support::JsonValue::object_t{
                                        {"text", sanitize_text(*text)},
                                        {"type", "input_text"},
                                }}},
                        {"role", "user"},
                });
            } else {
                auto content = response_content(std::get<std::vector<Content>>(user->content));
                if (!content.empty()) {
                    result.emplace_back(support::JsonValue::object_t{
                            {"content", std::move(content)},
                            {"role", "user"},
                    });
                }
            }
        } else if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            std::size_t text_index = 0;
            for (const auto& block : assistant->content) {
                if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                    if (thinking->thinking_signature) {
                        if (thinking->thinking_signature->empty()) {
                            continue;
                        }
                        auto replay = support::read_json(*thinking->thinking_signature);
                        if (!replay) {
                            return std::unexpected(support::make_error(support::ErrorCode::Stream,
                                    "Invalid Responses thinking signature",
                                    replay.error().detail));
                        }
                        result.push_back(std::move(*replay));
                    }
                } else if (const auto* text = std::get_if<TextContent>(&block)) {
                    const auto parsed = parse_text_signature(text->text_signature);
                    auto id = parsed && !parsed->id.empty() ? parsed->id
                              : text_index == 0
                                      ? "msg_pi_" + std::to_string(message_index)
                                      : "msg_pi_" + std::to_string(message_index) + "_" + std::to_string(text_index);
                    ++text_index;
                    support::JsonValue::object_t output{
                            {"content",
                                    support::JsonValue::array_t{support::JsonValue::object_t{
                                            {"annotations", support::JsonValue::array_t{}},
                                            {"text", sanitize_text(text->text)},
                                            {"type", "output_text"},
                                    }}},
                            {"id", bounded_message_id(std::move(id))},
                            {"role", "assistant"},
                            {"status", "completed"},
                            {"type", "message"},
                    };
                    if (parsed && parsed->phase) {
                        output.emplace("phase", *parsed->phase);
                    }
                    result.emplace_back(std::move(output));
                } else if (const auto* call = std::get_if<ToolCallContent>(&block)) {
                    const auto separator = call->id.find('|');
                    const auto call_id = separator == std::string::npos ? call->id : call->id.substr(0, separator);
                    const auto item_id = separator == std::string::npos
                                                 ? std::optional<std::string>{}
                                                 : std::optional<std::string>{call->id.substr(separator + 1)};
                    const auto arguments = call->arguments
                                                   ? support::write_json(*call->arguments)
                                                   : support::Expected<std::string>{
                                                             call->raw_arguments.empty() ? "{}" : call->raw_arguments};
                    if (!arguments) {
                        return std::unexpected(arguments.error());
                    }
                    support::JsonValue::object_t output{
                            {"arguments", *arguments},
                            {"call_id", call_id},
                            {"name", call->name},
                            {"type", "function_call"},
                    };
                    const bool different_model = assistant->model != model.id &&
                                                 assistant->provider == model.provider && assistant->api == model.api;
                    if (item_id && item_id->starts_with("fc_") && !different_model) {
                        output.emplace("id", *item_id);
                    }
                    result.emplace_back(std::move(output));
                }
            }
        } else if (const auto* tool_result = std::get_if<ToolResultMessage>(&message)) {
            const auto separator = tool_result->tool_call_id.find('|');
            const auto call_id = separator == std::string::npos ? tool_result->tool_call_id
                                                                : tool_result->tool_call_id.substr(0, separator);
            result.emplace_back(support::JsonValue::object_t{
                    {"call_id", call_id},
                    {"output", response_tool_output(model, tool_result->content)},
                    {"type", "function_call_output"},
            });
        }
        ++message_index;
    }
    return result;
}

[[nodiscard]] bool responses_supports_strict_mode(const Model& model) {
    if (!model.compat) {
        return false;
    }
    const auto* compat = std::get_if<OpenAIResponsesCompat>(&*model.compat);
    return compat != nullptr && compat->supports_strict_mode.value_or(false);
}

[[nodiscard]] bool responses_supports_explicit_prompt_cache_mode(const Model& model) {
    if (!model.compat) {
        return false;
    }
    const auto* compat = std::get_if<OpenAIResponsesCompat>(&*model.compat);
    return compat != nullptr && compat->supports_explicit_prompt_cache_mode.value_or(false);
}

[[nodiscard]] support::JsonValue::array_t responses_tools(
        AdapterKind adapter, const Model& model, const std::vector<Tool>& tools) {
    support::JsonValue::array_t result;
    for (const auto& tool : tools) {
        support::JsonValue::object_t converted{
                {"description", sanitize_text(tool.description)},
                {"name", tool.name},
                {"parameters", tool.parameters},
                {"type", "function"},
        };
        if (adapter == AdapterKind::OpenAICodexResponses) {
            converted.emplace("strict", nullptr);
        } else if (adapter == AdapterKind::OpenAIResponses && responses_supports_strict_mode(model)) {
            converted.emplace("strict", support::JsonValue{false});
        }
        result.emplace_back(std::move(converted));
    }
    return result;
}

[[nodiscard]] std::optional<std::string> responses_effort(
        const Model& model, std::optional<ModelThinkingLevel> reasoning) {
    if (!reasoning) {
        return std::nullopt;
    }
    const auto level = clamp_thinking_level(model, *reasoning);
    if (model.thinking_level_map) {
        if (const auto found = model.thinking_level_map->find(level); found != model.thinking_level_map->end()) {
            return found->second;
        }
    }
    switch (level) {
    case ModelThinkingLevel::Off:
        return "none";
    case ModelThinkingLevel::Minimal:
        return "minimal";
    case ModelThinkingLevel::Low:
        return "low";
    case ModelThinkingLevel::Medium:
        return "medium";
    case ModelThinkingLevel::High:
        return "high";
    case ModelThinkingLevel::XHigh:
        return "xhigh";
    case ModelThinkingLevel::Max:
        return "max";
    }
    return std::nullopt;
}

} // namespace

[[nodiscard]] support::Expected<support::JsonValue> build_responses_payload(
        AdapterKind adapter, const Model& model, const AiContext& context, const ProviderStreamOptions& options) {
    auto input = convert_responses_messages(adapter, model, context);
    if (!input) {
        return std::unexpected(input.error());
    }
    support::JsonValue::object_t payload{
            {"input", std::move(*input)},
            {"model", model.id},
            {"store", false},
            {"stream", true},
    };
    if (adapter == AdapterKind::OpenAICodexResponses) {
        payload.emplace("include", support::JsonValue::array_t{"reasoning.encrypted_content"});
        payload.emplace("instructions",
                context.system_prompt && !context.system_prompt->empty() ? sanitize_text(*context.system_prompt)
                                                                         : "You are a helpful assistant.");
        payload.emplace("parallel_tool_calls", true);
        payload.emplace("text", support::JsonValue::object_t{{"verbosity", "low"}});
        payload.emplace("tool_choice", "auto");
    } else {
        payload.emplace("max_output_tokens", static_cast<double>(std::max<std::uint64_t>(16, options.max_tokens)));
        if (options.cache_retention == CacheRetention::Long) {
            payload.emplace("prompt_cache_retention", "24h");
        }
        if (options.cache_retention == CacheRetention::None && responses_supports_explicit_prompt_cache_mode(model)) {
            payload.emplace("prompt_cache_options", support::JsonValue::object_t{{"mode", "explicit"}});
        }
    }
    if (options.session_id && options.cache_retention != CacheRetention::None) {
        payload.emplace("prompt_cache_key", detail::clamp_openai_prompt_cache_key(*options.session_id));
    }
    if (options.temperature) {
        payload.emplace("temperature", *options.temperature);
    }
    if (!context.tools.empty()) {
        payload.emplace("tools", responses_tools(adapter, model, context.tools));
    }
    if (model.reasoning) {
        auto effort = responses_effort(model, options.reasoning);
        if (!effort && adapter == AdapterKind::OpenAIResponses && !options.reasoning) {
            if (reasoning_off_supported(model)) {
                effort = model.thinking_level_map && model.thinking_level_map->contains(ModelThinkingLevel::Off)
                                 ? model.thinking_level_map->at(ModelThinkingLevel::Off)
                                 : std::optional<std::string>{"none"};
            }
        }
        if (effort) {
            support::JsonValue::object_t reasoning{{"effort", *effort}};
            if (options.reasoning) {
                reasoning.emplace("summary", "auto");
            }
            payload.emplace("reasoning", std::move(reasoning));
            if (adapter == AdapterKind::OpenAIResponses && options.reasoning &&
                    options.reasoning != ModelThinkingLevel::Off) {
                payload.emplace("include", support::JsonValue::array_t{"reasoning.encrypted_content"});
            }
        }
    }
    return support::JsonValue{std::move(payload)};
}

support::Expected<support::JsonValue::array_t> build_responses_continuation_items(
        const Model& model, const AssistantMessage& assistant) {
    AiContext context;
    context.messages.emplace_back(assistant);
    return convert_responses_messages(AdapterKind::OpenAICodexResponses, model, context);
}

} // namespace cch::ai::api
