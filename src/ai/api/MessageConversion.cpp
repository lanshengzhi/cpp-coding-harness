#include "MessageConversion.hpp"

#include "ai/SimpleOptions.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::ai::api {
namespace {

constexpr std::string_view kUserImagePlaceholder =
    "(image omitted: model does not support images)";
constexpr std::string_view kToolImagePlaceholder =
    "(tool image omitted: model does not support images)";
constexpr std::string_view kUtf8Replacement = "\xef\xbf\xbd";

[[nodiscard]] bool supports_images(const Model& model) {
    return std::ranges::find(model.input, ModelInput::Image) != model.input.end();
}

[[nodiscard]] std::size_t utf8_sequence_length(unsigned char lead) {
    if (lead <= 0x7f) {
        return 1;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        return 2;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
        return 3;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
        return 4;
    }
    return 0;
}

[[nodiscard]] bool valid_utf8_sequence(
    std::string_view text,
    std::size_t index,
    std::size_t length) {
    if (index + length > text.size()) {
        return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if (continuation < 0x80 || continuation > 0xbf) {
            return false;
        }
    }
    const auto lead = static_cast<unsigned char>(text[index]);
    if (length == 3) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        return !((lead == 0xe0 && second < 0xa0) ||
                 (lead == 0xed && second >= 0xa0));
    }
    if (length == 4) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        return !((lead == 0xf0 && second < 0x90) ||
                 (lead == 0xf4 && second >= 0x90));
    }
    return true;
}

[[nodiscard]] std::uint32_t decode_utf8_code_point(
    std::string_view value,
    std::size_t index,
    std::size_t length) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (length == 1) {
        return lead;
    }
    std::uint32_t result = lead & (length == 2 ? 0x1fU : length == 3 ? 0x0fU : 0x07U);
    for (std::size_t offset = 1; offset < length; ++offset) {
        result = (result << 6U) | (static_cast<unsigned char>(value[index + offset]) & 0x3fU);
    }
    return result;
}

[[nodiscard]] std::string sanitize_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
        if (length == 0) {
            result += kUtf8Replacement;
            ++index;
            continue;
        }
        if (!valid_utf8_sequence(text, index, length)) {
            result += kUtf8Replacement;
            std::size_t consumed = 1;
            while (consumed < length && index + consumed < text.size()) {
                const auto continuation = static_cast<unsigned char>(text[index + consumed]);
                if (continuation < 0x80 || continuation > 0xbf) {
                    break;
                }
                ++consumed;
            }
            index += consumed;
            continue;
        }
        result.append(text, index, length);
        index += length;
    }
    return result;
}

[[nodiscard]] bool blank(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
        if (length == 0 || !valid_utf8_sequence(text, index, length)) {
            return false;
        }
        const auto code_point = decode_utf8_code_point(text, index, length);
        const bool whitespace = code_point == 0x0009U || code_point == 0x000aU ||
                                code_point == 0x000bU || code_point == 0x000cU ||
                                code_point == 0x000dU || code_point == 0x0020U ||
                                code_point == 0x00a0U || code_point == 0x1680U ||
                                (code_point >= 0x2000U && code_point <= 0x200aU) ||
                                code_point == 0x2028U || code_point == 0x2029U ||
                                code_point == 0x202fU || code_point == 0x205fU ||
                                code_point == 0x3000U || code_point == 0xfeffU;
        if (!whitespace) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] std::string normalize_id_part(std::string_view value, bool trim_trailing) {
    std::string result;
    result.reserve(std::min<std::size_t>(64, value.size()));
    for (std::size_t index = 0; index < value.size() && result.size() < 64;) {
        const auto character = static_cast<unsigned char>(value[index]);
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '_' || character == '-';
        if (allowed) {
            result.push_back(static_cast<char>(character));
            ++index;
            continue;
        }
        const auto length = utf8_sequence_length(character);
        const bool valid = length != 0 && valid_utf8_sequence(value, index, length);
        const auto replacement_count = valid &&
                decode_utf8_code_point(value, index, length) > 0xffffU
            ? 2U
            : 1U;
        result.append(std::min<std::size_t>(replacement_count, 64 - result.size()), '_');
        index += valid ? length : 1;
    }
    if (trim_trailing) {
        while (!result.empty() && result.back() == '_') {
            result.pop_back();
        }
    }
    return result;
}


[[nodiscard]] std::string normalize_tool_call_id(
    AdapterKind adapter,
    const Model& target,
    const AssistantMessage& source,
    std::string_view value);

[[nodiscard]] std::vector<Content> downgrade_images(
    const std::vector<Content>& content,
    std::string_view placeholder,
    bool image_capable) {
    if (image_capable) {
        return content;
    }
    std::vector<Content> result;
    bool previous_placeholder = false;
    for (const auto& block : content) {
        if (std::holds_alternative<ImageContent>(block)) {
            if (!previous_placeholder) {
                result.emplace_back(text_content(std::string{placeholder}));
            }
            previous_placeholder = true;
            continue;
        }
        result.push_back(block);
        const auto* text = std::get_if<TextContent>(&block);
        previous_placeholder = text && text->text == placeholder;
    }
    return result;
}

[[nodiscard]] std::vector<MessageVariant> normalize_history(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context) {
    std::vector<MessageVariant> result;
    std::map<std::string, std::string, std::less<>> normalized_ids;
    std::vector<ToolCallContent> pending_calls;
    std::set<std::string, std::less<>> result_ids;

    const auto flush_orphans = [&]() {
        for (const auto& call : pending_calls) {
            if (!result_ids.contains(call.id)) {
                result.emplace_back(ToolResultMessage{
                    .tool_call_id = call.id,
                    .tool_name = call.name,
                    .content = {text_content("No result provided")},
                    .details = std::nullopt,
                    .is_error = true,
                    .timestamp = 0,
                });
            }
        }
        pending_calls.clear();
        result_ids.clear();
    };

    for (const auto& message : context.messages) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            flush_orphans();
            if (assistant->stop_reason == AssistantStopReason::Error ||
                assistant->stop_reason == AssistantStopReason::Aborted) {
                continue;
            }
            auto transformed = *assistant;
            transformed.content.clear();
            const bool same_model = assistant->provider == model.provider &&
                                    assistant->api == model.api &&
                                    assistant->model == model.id;
            for (const auto& block : assistant->content) {
                if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                    if (thinking->redacted) {
                        if (same_model) {
                            transformed.content.emplace_back(*thinking);
                        }
                    } else if (same_model && thinking->thinking_signature &&
                               !thinking->thinking_signature->empty()) {
                        transformed.content.emplace_back(*thinking);
                    } else if (!blank(thinking->thinking)) {
                        if (same_model) {
                            transformed.content.emplace_back(*thinking);
                        } else {
                            transformed.content.emplace_back(TextContent{
                                .text = thinking->thinking,
                                .text_signature = std::nullopt,
                            });
                        }
                    }
                    continue;
                }
                if (const auto* text = std::get_if<TextContent>(&block)) {
                    auto converted = *text;
                    if (!same_model) {
                        converted.text_signature = std::nullopt;
                    }
                    transformed.content.emplace_back(std::move(converted));
                    continue;
                }
                auto call = std::get<ToolCallContent>(block);
                const auto original_id = call.id;
                if (!same_model) {
                    call.id = normalize_tool_call_id(adapter, model, *assistant, call.id);
                    if (call.id != original_id) {
                        normalized_ids.insert_or_assign(original_id, call.id);
                    }
                    call.thought_signature = std::nullopt;
                }
                pending_calls.push_back(call);
                transformed.content.emplace_back(std::move(call));
            }
            result.emplace_back(std::move(transformed));
            continue;
        }

        if (const auto* tool_result = std::get_if<ToolResultMessage>(&message)) {
            auto transformed = *tool_result;
            if (const auto found = normalized_ids.find(transformed.tool_call_id);
                found != normalized_ids.end()) {
                transformed.tool_call_id = found->second;
            }
            transformed.content = downgrade_images(
                transformed.content, kToolImagePlaceholder, supports_images(model));
            result_ids.insert(transformed.tool_call_id);
            result.emplace_back(std::move(transformed));
            continue;
        }

        flush_orphans();
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            auto transformed = *user;
            if (const auto* blocks =
                    std::get_if<std::vector<Content>>(&transformed.content)) {
                // pi `downgradeUnsupportedImages` guards on `Array.isArray`:
                // the string alternative passes through untouched.
                transformed.content = downgrade_images(
                    *blocks, kUserImagePlaceholder, supports_images(model));
            }
            result.emplace_back(std::move(transformed));
        } else if (const auto* bash = std::get_if<BashExecutionMessage>(&message)) {
            if (!bash->exclude_from_context) {
                result.emplace_back(bash_execution_to_user_message(*bash));
            }
        } else if (const auto* custom = std::get_if<CustomMessage>(&message)) {
            result.emplace_back(custom_message_to_user_message(*custom));
        } else if (const auto* branch = std::get_if<BranchSummaryMessage>(&message)) {
            result.emplace_back(branch_summary_to_user_message(*branch));
        } else if (const auto* compaction = std::get_if<CompactionSummaryMessage>(&message)) {
            result.emplace_back(compaction_summary_to_user_message(*compaction));
        }
    }
    flush_orphans();
    return result;
}

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

[[nodiscard]] support::JsonValue response_tool_output(
    const Model& model,
    const std::vector<Content>& content) {
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

[[nodiscard]] std::optional<ParsedTextSignature> parse_text_signature(
    const std::optional<std::string>& signature) {
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
                if (version != object->end() && id != object->end() &&
                    version->second.holds<double>() && version->second.get_number() == 1 &&
                    id->second.holds<std::string>()) {
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

[[nodiscard]] std::string base36(std::uint32_t value) {
    constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string result;
    do {
        result.push_back(kDigits[value % 36]);
        value /= 36;
    } while (value != 0);
    std::ranges::reverse(result);
    return result;
}

[[nodiscard]] std::uint32_t multiply_wrapped(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(left) * static_cast<std::uint64_t>(right));
}

[[nodiscard]] std::string short_hash(std::string_view value) {
    std::uint32_t first = 0xdeadbeefU;
    std::uint32_t second = 0x41c6ce57U;
    const auto mix = [&first, &second](std::uint32_t code_unit) {
        first = multiply_wrapped(first ^ code_unit, 2654435761U);
        second = multiply_wrapped(second ^ code_unit, 1597334677U);
    };
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(value[index]));
        if (length == 0 || !valid_utf8_sequence(value, index, length)) {
            mix(0xfffdU);
            ++index;
            continue;
        }
        const auto code_point = decode_utf8_code_point(value, index, length);
        if (code_point <= 0xffffU) {
            mix(code_point);
        } else {
            const auto supplementary = code_point - 0x10000U;
            mix(0xd800U + (supplementary >> 10U));
            mix(0xdc00U + (supplementary & 0x3ffU));
        }
        index += length;
    }
    first = multiply_wrapped(first ^ (first >> 16), 2246822507U) ^
            multiply_wrapped(second ^ (second >> 13), 3266489909U);
    second = multiply_wrapped(second ^ (second >> 16), 2246822507U) ^
             multiply_wrapped(first ^ (first >> 13), 3266489909U);
    return base36(second) + base36(first);
}

[[nodiscard]] std::size_t utf16_length(std::string_view value) {
    std::size_t result = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(value[index]));
        if (length == 0 || !valid_utf8_sequence(value, index, length)) {
            ++result;
            ++index;
            continue;
        }
        result += decode_utf8_code_point(value, index, length) > 0xffffU ? 2 : 1;
        index += length;
    }
    return result;
}

[[nodiscard]] std::string bounded_message_id(std::string id) {
    return utf16_length(id) <= 64 ? id : "msg_" + short_hash(id);
}

[[nodiscard]] bool responses_tool_call_provider(std::string_view provider) {
    return provider == "openai" || provider == "openai-codex" || provider == "opencode";
}

[[nodiscard]] std::string normalize_tool_call_id(
    AdapterKind adapter,
    const Model& target,
    const AssistantMessage& source,
    std::string_view value) {
    if (adapter == AdapterKind::AnthropicMessages) {
        return normalize_id_part(value, false);
    }
    if (!responses_tool_call_provider(target.provider)) {
        return normalize_id_part(value, true);
    }
    const auto separator = value.find('|');
    if (separator == std::string_view::npos) {
        return normalize_id_part(value, true);
    }
    auto call_id = normalize_id_part(value.substr(0, separator), true);
    const auto item_begin = separator + 1;
    const auto next_separator = value.find('|', item_begin);
    const auto raw_item_id = value.substr(
        item_begin,
        next_separator == std::string_view::npos
            ? std::string_view::npos
            : next_separator - item_begin);
    const bool foreign_api = source.provider != target.provider || source.api != target.api;
    auto item_id = foreign_api
        ? "fc_" + short_hash(raw_item_id)
        : normalize_id_part(raw_item_id, true);
    if (!item_id.starts_with("fc_")) {
        item_id = normalize_id_part("fc_" + item_id, true);
    }
    if (item_id.size() > 64) {
        item_id.resize(64);
    }
    return call_id + "|" + item_id;
}

[[nodiscard]] support::Expected<support::JsonValue::array_t> convert_responses_messages(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context) {
    support::JsonValue::array_t result;
    if (adapter == AdapterKind::OpenAIResponses && context.system_prompt &&
        !context.system_prompt->empty()) {
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
                    {"content", support::JsonValue::array_t{support::JsonValue::object_t{
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
                        auto replay = support::read_json(
                            *thinking->thinking_signature);
                        if (!replay) {
                            return std::unexpected(support::make_error(
                                support::ErrorCode::Stream,
                                "Invalid Responses thinking signature",
                                replay.error().detail));
                        }
                        result.push_back(std::move(*replay));
                    }
                } else if (const auto* text = std::get_if<TextContent>(&block)) {
                    const auto parsed = parse_text_signature(text->text_signature);
                    auto id = parsed && !parsed->id.empty()
                        ? parsed->id
                        : text_index == 0
                            ? "msg_pi_" + std::to_string(message_index)
                            : "msg_pi_" + std::to_string(message_index) + "_" +
                                  std::to_string(text_index);
                    ++text_index;
                    support::JsonValue::object_t output{
                        {"content", support::JsonValue::array_t{support::JsonValue::object_t{
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
                    const auto call_id = separator == std::string::npos
                        ? call->id
                        : call->id.substr(0, separator);
                    const auto item_id = separator == std::string::npos
                        ? std::optional<std::string>{}
                        : std::optional<std::string>{call->id.substr(separator + 1)};
                    const auto arguments = call->arguments
                        ? support::write_json(*call->arguments)
                        : support::Expected<std::string>{call->raw_arguments.empty() ? "{}" : call->raw_arguments};
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
                                                 assistant->provider == model.provider &&
                                                 assistant->api == model.api;
                    if (item_id && item_id->starts_with("fc_") && !different_model) {
                        output.emplace("id", *item_id);
                    }
                    result.emplace_back(std::move(output));
                }
            }
        } else if (const auto* tool_result = std::get_if<ToolResultMessage>(&message)) {
            const auto separator = tool_result->tool_call_id.find('|');
            const auto call_id = separator == std::string::npos
                ? tool_result->tool_call_id
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

[[nodiscard]] support::JsonValue::array_t responses_tools(
    AdapterKind adapter,
    const std::vector<Tool>& tools) {
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
        }
        result.emplace_back(std::move(converted));
    }
    return result;
}

[[nodiscard]] bool reasoning_off_supported(const Model& model) {
    if (!model.thinking_level_map) {
        return true;
    }
    const auto found = model.thinking_level_map->find(ModelThinkingLevel::Off);
    return found == model.thinking_level_map->end() || found->second.has_value();
}

[[nodiscard]] std::optional<std::string> responses_effort(
    const Model& model,
    std::optional<ModelThinkingLevel> reasoning) {
    if (!reasoning) {
        return std::nullopt;
    }
    const auto level = clamp_thinking_level(model, *reasoning);
    if (model.thinking_level_map) {
        if (const auto found = model.thinking_level_map->find(level);
            found != model.thinking_level_map->end()) {
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

[[nodiscard]] support::Expected<support::JsonValue> build_responses_payload(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
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
        payload.emplace(
            "include",
            support::JsonValue::array_t{"reasoning.encrypted_content"});
        payload.emplace(
            "instructions",
            context.system_prompt && !context.system_prompt->empty()
                ? sanitize_text(*context.system_prompt)
                : "You are a helpful assistant.");
        payload.emplace("parallel_tool_calls", true);
        payload.emplace("text", support::JsonValue::object_t{{"verbosity", "low"}});
        payload.emplace("tool_choice", "auto");
    } else {
        payload.emplace("max_output_tokens", static_cast<double>(std::max<std::uint64_t>(16, options.max_tokens)));
        if (options.cache_retention == CacheRetention::Long) {
            payload.emplace("prompt_cache_retention", "24h");
        }
    }
    if (options.session_id && options.cache_retention != CacheRetention::None) {
        payload.emplace(
            "prompt_cache_key",
            detail::clamp_openai_prompt_cache_key(*options.session_id));
    }
    if (options.temperature) {
        payload.emplace("temperature", *options.temperature);
    }
    if (!context.tools.empty()) {
        payload.emplace("tools", responses_tools(adapter, context.tools));
    }
    if (model.reasoning) {
        auto effort = responses_effort(model, options.reasoning);
        if (!effort && adapter == AdapterKind::OpenAIResponses && !options.reasoning) {
            if (reasoning_off_supported(model)) {
                effort = model.thinking_level_map &&
                         model.thinking_level_map->contains(ModelThinkingLevel::Off)
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
            if (adapter == AdapterKind::OpenAIResponses &&
                options.reasoning && options.reasoning != ModelThinkingLevel::Off) {
                payload.emplace(
                    "include",
                    support::JsonValue::array_t{"reasoning.encrypted_content"});
            }
        }
    }
    return support::JsonValue{std::move(payload)};
}

[[nodiscard]] support::JsonValue anthropic_image(const ImageContent& image) {
    return support::JsonValue::object_t{
        {"source", support::JsonValue::object_t{
            {"data", image.data},
            {"media_type", image.mime_type},
            {"type", "base64"},
        }},
        {"type", "image"},
    };
}

[[nodiscard]] support::JsonValue::array_t anthropic_user_content(
    const std::vector<Content>& content) {
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

[[nodiscard]] support::JsonValue anthropic_tool_result_content(
    const std::vector<Content>& content) {
    const bool has_images = std::ranges::any_of(content, [](const Content& block) {
        return std::holds_alternative<ImageContent>(block);
    });
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
        blocks.insert(
            blocks.begin(),
            support::JsonValue::object_t{{"text", "(see attached image)"}, {"type", "text"}});
    }
    return blocks;
}

[[nodiscard]] support::Expected<support::JsonValue::array_t> convert_anthropic_messages(
    const Model& model,
    const AiContext& context) {
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
                        const bool has_signature = thinking->thinking_signature &&
                                                   !blank(*thinking->thinking_signature);
                        if (blank(thinking->thinking) && !has_signature) {
                            continue;
                        }
                        const bool allow_empty = model.compat &&
                            model.compat->allow_empty_signature.value_or(false);
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

void attach_cache_control_to_last_user(
    support::JsonValue::array_t& messages,
    CacheRetention retention) {
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
            blocks->back().get_object().insert_or_assign(
                "cache_control", cache_control(retention));
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

[[nodiscard]] support::JsonValue::array_t anthropic_tools(
    const std::vector<Tool>& tools,
    CacheRetention retention) {
    support::JsonValue::array_t result;
    for (std::size_t index = 0; index < tools.size(); ++index) {
        const auto& tool = tools[index];
        support::JsonValue::object_t schema{{"type", "object"}};
        if (const auto* parameters = tool.parameters.get_if<support::JsonValue::object_t>()) {
            if (const auto properties = parameters->find("properties");
                properties != parameters->end()) {
                schema.emplace("properties", properties->second);
            } else {
                schema.emplace("properties", support::JsonValue::object_t{});
            }
            if (const auto required = parameters->find("required");
                required != parameters->end()) {
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

[[nodiscard]] std::string anthropic_effort(
    const Model& model,
    ModelThinkingLevel level) {
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

[[nodiscard]] support::Expected<support::JsonValue> build_anthropic_payload(
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
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
            system_block.emplace(
                "cache_control", cache_control(options.cache_retention));
        }
        payload.emplace(
            "system",
            support::JsonValue::array_t{std::move(system_block)});
    }
    bool thinking_enabled = false;
    if (model.reasoning && !options.reasoning) {
        if (reasoning_off_supported(model)) {
            payload.emplace(
                "thinking", support::JsonValue::object_t{{"type", "disabled"}});
        }
    } else if (model.reasoning && options.reasoning) {
        const auto level = clamp_thinking_level(model, *options.reasoning);
        if (level == ModelThinkingLevel::Off) {
            if (reasoning_off_supported(model)) {
                payload.emplace(
                    "thinking", support::JsonValue::object_t{{"type", "disabled"}});
            }
        } else if (model.compat &&
                   model.compat->force_adaptive_thinking.value_or(false)) {
            thinking_enabled = true;
            payload.emplace(
                "thinking",
                support::JsonValue::object_t{
                    {"display", "summarized"},
                    {"type", "adaptive"},
                });
            payload.emplace(
                "output_config",
                support::JsonValue::object_t{{"effort", anthropic_effort(model, level)}});
        } else {
            return std::unexpected(support::make_error(
                support::ErrorCode::ModelValidation,
                "Budget-based Anthropic thinking is outside the supported adapter surface"));
        }
    }
    if (options.temperature && !thinking_enabled) {
        payload.emplace("temperature", *options.temperature);
    }
    if (!context.tools.empty()) {
        payload.emplace(
            "tools", anthropic_tools(context.tools, options.cache_retention));
    }
    return support::JsonValue{std::move(payload)};
}

} // namespace

support::Expected<support::JsonValue> build_adapter_payload(
    AdapterKind adapter,
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
    if (adapter == AdapterKind::AnthropicMessages) {
        return build_anthropic_payload(model, context, options);
    }
    return build_responses_payload(adapter, model, context, options);
}

support::Expected<support::JsonValue::array_t> build_responses_continuation_items(
    const Model& model,
    const AssistantMessage& assistant) {
    AiContext context;
    context.messages.emplace_back(assistant);
    return convert_responses_messages(AdapterKind::OpenAICodexResponses, model, context);
}

} // namespace cch::ai::api
