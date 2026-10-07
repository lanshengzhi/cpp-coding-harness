#include "ResponsesSlots.hpp"

#include "ai/JsonAccess.hpp"
#include <cch/ai/Timestamps.hpp>
#include "ai/api/PartialJson.hpp"
#include "ai/api/Termination.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/Json.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::api {
namespace {

[[nodiscard]] std::string joined_item_text(const JsonObject& item, std::string_view array_name) {
    const auto* entries = json_array_member(item, array_name);
    if (!entries) {
        return {};
    }
    std::string result;
    for (const auto& entry : *entries) {
        const auto* entry_object = entry.get_if<JsonObject>();
        if (!entry_object) {
            continue;
        }
        const auto text = json_string_member(*entry_object, "text");
        const auto refusal = json_string_member(*entry_object, "refusal");
        const auto part = text ? text : refusal;
        if (!part) {
            continue;
        }
        if (!result.empty()) {
            result += "\n\n";
        }
        result += *part;
    }
    return result;
}

[[nodiscard]] std::string joined_message_text(const JsonObject& item) {
    const auto* entries = json_array_member(item, "content");
    if (!entries) {
        return {};
    }
    std::string result;
    for (const auto& entry : *entries) {
        const auto* entry_object = entry.get_if<JsonObject>();
        if (!entry_object) {
            continue;
        }
        if (const auto text = json_string_member(*entry_object, "text")) {
            result += *text;
        } else if (const auto refusal = json_string_member(*entry_object, "refusal")) {
            result += *refusal;
        }
    }
    return result;
}

/// Text signature v1: {"id": <item-id>, "v": 1, "phase":
/// "commentary"|"final_answer"}.
[[nodiscard]] support::Expected<std::string> text_signature(const JsonObject& item) {
    support::JsonValue::object_t signature{
            {"id", std::string{json_string_member(item, "id").value_or("")}},
            {"v", 1},
    };
    if (const auto phase = json_string_member(item, "phase"); phase == "commentary" || phase == "final_answer") {
        signature.emplace("phase", std::string{*phase});
    }
    return support::write_json(support::JsonValue{std::move(signature)});
}

void apply_message_phase_stop_reason(AssistantMessage& assistant, const JsonObject& item) {
    // pi's `applyMessagePhaseStopReason` (openai-responses-shared.ts): a
    // message output item whose `phase` is `final_answer` flips the running
    // partial from `pending` to `stop` before the item is surfaced.
    if (const auto type = json_string_member(item, "type"); type && *type == "message") {
        if (const auto phase = json_string_member(item, "phase"); phase && *phase == "final_answer") {
            assistant.stop_reason = AssistantStopReason::Stop;
        }
    }
}

} // namespace

[[nodiscard]] std::optional<std::size_t> output_index(const JsonObject& event) {
    const auto value = json_integer_member(event, "output_index");
    return value ? std::optional<std::size_t>{static_cast<std::size_t>(*value)} : std::nullopt;
}

[[nodiscard]] support::ExpectedVoid create_slot(std::size_t index,
        const JsonObject& item,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        const std::map<std::string, std::string, std::less<>>* grammar_properties) {
    if (slots.contains(index)) {
        return {};
    }
    const auto type = json_string_member(item, "type");
    if (!type) {
        return {};
    }
    if (*type == "reasoning") {
        const auto content_index = assistant.content.size();
        assistant.content.emplace_back(ThinkingContent{});
        slots.emplace(index,
                Slot{
                        .kind = Slot::Kind::Thinking,
                        .content_index = content_index,
                        .partial_arguments = {},
                });
        return providers::emit(sink,
                ThinkingStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    if (*type == "message") {
        apply_message_phase_stop_reason(assistant, item);
        const auto content_index = assistant.content.size();
        assistant.content.emplace_back(TextContent{});
        slots.emplace(index,
                Slot{
                        .kind = Slot::Kind::Text,
                        .content_index = content_index,
                        .partial_arguments = {},
                });
        return providers::emit(sink,
                TextStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    if (*type == "function_call") {
        const auto content_index = assistant.content.size();
        auto arguments = std::string{json_string_member(item, "arguments").value_or("")};
        assistant.content.emplace_back(ToolCallContent{
                .id = std::string{json_string_member(item, "call_id").value_or("")} + "|" +
                      std::string{json_string_member(item, "id").value_or("")},
                .name = std::string{json_string_member(item, "name").value_or("")},
                // pi constructs every tool call with an empty arguments object
                // (parseStreamingJson("")) and fills it from raw arguments.
                .arguments = support::JsonValue{support::JsonValue::object_t{}},
                .raw_arguments = arguments,
                .thought_signature = std::nullopt,
                .arguments_valid = true,
                .argument_error = std::nullopt,
        });
        slots.emplace(index,
                Slot{
                        .kind = Slot::Kind::ToolCall,
                        .content_index = content_index,
                        .partial_arguments = std::move(arguments),
                });
        return providers::emit(sink,
                ToolCallStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    if (*type == "custom_tool_call") {
        // pi `openai-responses-shared`: a grammar-constrained tool call carries
        // its raw input under the tool's grammar input property.
        const std::string name = std::string{json_string_member(item, "name").value_or("")};
        std::string property = "input";
        if (grammar_properties != nullptr) {
            if (const auto found = grammar_properties->find(name); found != grammar_properties->end()) {
                property = found->second;
            }
        }
        const std::string input = std::string{json_string_member(item, "input").value_or("")};
        auto arguments = support::JsonValue{support::JsonValue::object_t{{property, input}}};
        auto raw = support::write_json(arguments);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        const auto content_index = assistant.content.size();
        assistant.content.emplace_back(ToolCallContent{
                .id = std::string{json_string_member(item, "call_id").value_or("")} + "|" +
                      std::string{json_string_member(item, "id").value_or("")},
                .name = name,
                .arguments = std::move(arguments),
                .raw_arguments = std::move(*raw),
                .thought_signature = std::nullopt,
                .arguments_valid = true,
                .argument_error = std::nullopt,
        });
        Slot slot{
                .kind = Slot::Kind::ToolCall,
                .content_index = content_index,
                .partial_arguments = std::get<ToolCallContent>(assistant.content[content_index]).raw_arguments,
        };
        slot.custom_property = property;
        slot.custom_buffer.input = input;
        slots.emplace(index, std::move(slot));
        return providers::emit(sink,
                ToolCallStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid append_delta(const JsonObject& event,
        std::string_view type,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto index = output_index(event);
    const auto delta = json_string_member(event, "delta");
    if (!index || !delta) {
        return {};
    }
    const auto found = slots.find(*index);
    if (found == slots.end()) {
        return {};
    }
    auto& slot = found->second;
    if ((type == "response.reasoning_summary_text.delta" || type == "response.reasoning_text.delta") &&
            slot.kind == Slot::Kind::Thinking) {
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        block.thinking += *delta;
        return providers::emit(sink,
                ThinkingDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{*delta},
                        .partial = assistant,
                });
    }
    if ((type == "response.output_text.delta" || type == "response.refusal.delta") && slot.kind == Slot::Kind::Text) {
        auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        block.text += *delta;
        return providers::emit(sink,
                TextDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{*delta},
                        .partial = assistant,
                });
    }
    if (type == "response.function_call_arguments.delta" && slot.kind == Slot::Kind::ToolCall) {
        slot.partial_arguments += *delta;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        block.raw_arguments = slot.partial_arguments;
        block.arguments = parse_streaming_json(block.raw_arguments);
        block.arguments_valid = true;
        block.argument_error = std::nullopt;
        return providers::emit(sink,
                ToolCallDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{*delta},
                        .partial = assistant,
                });
    }
    if (type == "response.custom_tool_call_input.delta" && slot.kind == Slot::Kind::ToolCall &&
            !slot.custom_property.empty()) {
        const std::string next_input = slot.custom_buffer.input + std::string{*delta};
        slot.custom_buffer.input = next_input;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        auto arguments = support::JsonValue{support::JsonValue::object_t{{slot.custom_property, next_input}}};
        auto raw = support::write_json(arguments);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        block.arguments = std::move(arguments);
        block.raw_arguments = std::move(*raw);
        block.arguments_valid = true;
        block.argument_error = std::nullopt;
        return providers::emit(sink,
                ToolCallDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{*delta},
                        .partial = assistant,
                });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid append_reasoning_separator(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto index = output_index(event);
    if (!index) {
        return {};
    }
    const auto found = slots.find(*index);
    if (found == slots.end() || found->second.kind != Slot::Kind::Thinking) {
        return {};
    }
    auto& block = std::get<ThinkingContent>(assistant.content[found->second.content_index]);
    block.thinking += "\n\n";
    return providers::emit(sink,
            ThinkingDeltaEvent{
                    .content_index = found->second.content_index,
                    .delta = "\n\n",
                    .partial = assistant,
            });
}

[[nodiscard]] support::ExpectedVoid finish_argument_stream(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto index = output_index(event);
    const auto arguments = json_string_member(event, "arguments");
    if (!index) {
        return {};
    }
    const auto found = slots.find(*index);
    if (found == slots.end()) {
        return {};
    }
    auto& slot = found->second;
    if (!arguments && !slot.custom_property.empty()) {
        // pi `response.custom_tool_call_input.done`: the final grammar input
        // closes the streamed `custom_tool_call` arguments.
        const auto input = json_string_member(event, "input");
        const std::string next_input = input ? std::string{*input} : slot.custom_buffer.input;
        slot.custom_buffer.input = next_input;
        slot.custom_buffer.closed = true;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        auto custom_arguments = support::JsonValue{support::JsonValue::object_t{{slot.custom_property, next_input}}};
        auto raw = support::write_json(custom_arguments);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        block.arguments = std::move(custom_arguments);
        block.raw_arguments = std::move(*raw);
        finalize_tool_arguments(block);
        // pi keeps the slot open until `response.output_item.done`; the done
        // input only closes the grammar buffer.
        return {};
    }
    if (!arguments || slot.kind != Slot::Kind::ToolCall) {
        return {};
    }
    const auto previous = slot.partial_arguments;
    slot.partial_arguments = *arguments;
    auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
    block.raw_arguments = slot.partial_arguments;
    block.arguments = parse_streaming_json(block.raw_arguments);
    block.arguments_valid = true;
    block.argument_error = std::nullopt;
    if (arguments->starts_with(previous) && arguments->size() > previous.size()) {
        return providers::emit(sink,
                ToolCallDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{arguments->substr(previous.size())},
                        .partial = assistant,
                });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid finish_output_item(const JsonObject& event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto index = output_index(event);
    const auto* item = json_object_member(event, "item");
    if (!index || !item) {
        return {};
    }
    if (auto created = create_slot(*index, *item, slots, assistant, sink); !created) {
        return std::unexpected(created.error());
    }
    const auto found = slots.find(*index);
    if (found == slots.end()) {
        return {};
    }
    const auto slot = found->second;
    const auto type = json_string_member(*item, "type");
    if (type == "reasoning" && slot.kind == Slot::Kind::Thinking) {
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        auto content = joined_item_text(*item, "summary");
        if (content.empty()) {
            content = joined_item_text(*item, "content");
        }
        if (!content.empty()) {
            block.thinking = std::move(content);
        }
        auto signature = support::write_json(support::JsonValue{*item});
        if (!signature) {
            return std::unexpected(signature.error());
        }
        block.thinking_signature = std::move(*signature);
        if (auto emitted = providers::emit(sink,
                    ThinkingEndEvent{
                            .content_index = slot.content_index,
                            .content = block.thinking,
                            .partial = assistant,
                    });
                !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
        return {};
    }
    if (type == "message" && slot.kind == Slot::Kind::Text) {
        apply_message_phase_stop_reason(assistant, *item);
        auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        block.text = joined_message_text(*item);
        auto signature = text_signature(*item);
        if (!signature) {
            return std::unexpected(signature.error());
        }
        block.text_signature = std::move(*signature);
        if (auto emitted = providers::emit(sink,
                    TextEndEvent{
                            .content_index = slot.content_index,
                            .content = block.text,
                            .partial = assistant,
                    });
                !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
        return {};
    }
    if (type == "function_call" && slot.kind == Slot::Kind::ToolCall) {
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        block.id = std::string{json_string_member(*item, "call_id").value_or("")} + "|" +
                   std::string{json_string_member(*item, "id").value_or("")};
        block.name = std::string{json_string_member(*item, "name").value_or("")};
        block.raw_arguments = std::string{json_string_member(*item, "arguments").value_or(slot.partial_arguments)};
        finalize_tool_arguments(block);
        if (auto emitted = providers::emit(sink,
                    ToolCallEndEvent{
                            .content_index = slot.content_index,
                            .tool_call = block,
                            .partial = assistant,
                    });
                !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
        return {};
    }
    if (type == "custom_tool_call" && slot.kind == Slot::Kind::ToolCall && !slot.custom_property.empty()) {
        const auto input = json_string_member(*item, "input");
        const std::string next_input = input ? std::string{*input} : slot.custom_buffer.input;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        auto custom_arguments = support::JsonValue{support::JsonValue::object_t{{slot.custom_property, next_input}}};
        auto raw = support::write_json(custom_arguments);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        block.arguments = std::move(custom_arguments);
        block.raw_arguments = std::move(*raw);
        finalize_tool_arguments(block);
        if (auto emitted = providers::emit(sink,
                    ToolCallEndEvent{
                            .content_index = slot.content_index,
                            .tool_call = block,
                            .partial = assistant,
                    });
                !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
    }
    return {};
}

std::optional<GrammarConstrainedSampling> resolve_grammar_constrained_sampling(
        const Tool& tool, bool supports_openai_grammar_tools) {
    if (!tool.constrained_sampling.has_value() || !supports_openai_grammar_tools) return std::nullopt;
    const auto& variants = tool.constrained_sampling->variants;
    const auto lark = variants.find("openai_lark");
    const auto regex = variants.find("openai_regex");
    const auto trimmed_empty = [](std::string_view text) {
        return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
    };
    const bool has_lark = lark != variants.end() && !trimmed_empty(lark->second);
    const bool has_regex = regex != variants.end() && !trimmed_empty(regex->second);
    if (!has_lark && !has_regex) return std::nullopt;

    // pi `inferGrammarInputProperty`: exactly one required string property.
    const auto* schema = tool.parameters.get_if<support::JsonValue::object_t>();
    if (schema == nullptr) return std::nullopt;
    const auto type = schema->find("type");
    if (type == schema->end() || !type->second.holds<std::string>() || type->second.get_string() != "object") {
        return std::nullopt;
    }
    const auto required = schema->find("required");
    if (required == schema->end() || !required->second.holds<support::JsonValue::array_t>() ||
            required->second.get_array().size() != 1 || !required->second.get_array().front().holds<std::string>()) {
        return std::nullopt;
    }
    const std::string property = required->second.get_array().front().get_string();
    const auto properties = schema->find("properties");
    if (properties == schema->end() || !properties->second.holds<support::JsonValue::object_t>()) return std::nullopt;
    const auto found = properties->second.get_object().find(property);
    if (found == properties->second.get_object().end()) return std::nullopt;
    const auto* property_schema = found->second.get_if<support::JsonValue::object_t>();
    if (property_schema == nullptr) return std::nullopt;
    const auto property_type = property_schema->find("type");
    if (property_type == property_schema->end() || !property_type->second.holds<std::string>() ||
            property_type->second.get_string() != "string") {
        return std::nullopt;
    }
    return GrammarConstrainedSampling{
            .format = has_lark ? "lark" : "regex",
            .definition = has_lark ? lark->second : regex->second,
            .input_property = property,
    };
}

bool model_supports_openai_grammar_tools(const Model& model) {
    if (!model.compat) return false;
    const auto* compat = std::get_if<OpenAIResponsesCompat>(&*model.compat);
    return compat != nullptr && compat->supports_openai_grammar_tools.value_or(false);
}

std::map<std::string, std::string, std::less<>> grammar_tool_input_properties(
        const std::vector<Tool>& tools, const Model& model) {
    std::map<std::string, std::string, std::less<>> properties;
    const bool capable = model_supports_openai_grammar_tools(model);
    for (const auto& tool : tools) {
        if (auto grammar = resolve_grammar_constrained_sampling(tool, capable); grammar.has_value()) {
            properties.emplace(tool.name, grammar->input_property);
        }
    }
    return properties;
}

std::optional<std::string> append_grammar_tool_input_json_delta(
        GrammarToolInputJsonBuffer& buffer, std::string_view input_property, std::string_view next_input, bool close) {
    if (buffer.closed) {
        if (close && next_input == buffer.input) return std::nullopt;
        return std::nullopt;
    }
    if (!next_input.starts_with(buffer.input)) return std::nullopt;
    const std::string_view delta = next_input.substr(buffer.input.size());
    if (!close && delta.empty()) return std::nullopt;

    std::string fragment;
    if (!buffer.started) {
        fragment += "{" + support::write_json(support::JsonValue{std::string{input_property}}).value_or("\"input\"") +
                    ":\"";
        buffer.started = true;
    }
    auto encoded = support::write_json(support::JsonValue{std::string{delta}});
    if (encoded && encoded->size() >= 2) fragment += encoded->substr(1, encoded->size() - 2);
    buffer.input = std::string{next_input};
    if (close) {
        fragment += "\"}";
        buffer.closed = true;
    }
    return fragment;
}

} // namespace cch::ai::api
