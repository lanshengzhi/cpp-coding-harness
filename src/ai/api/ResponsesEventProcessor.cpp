#include "ResponsesEventProcessor.hpp"

#include "ai/api/PartialJson.hpp"
#include "ai/api/Termination.hpp"
#include "ai/api/UsageNormalization.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::api {
namespace {

using JsonArray = support::JsonValue::array_t;

struct Slot {
    enum class Kind {
        Thinking,
        Text,
        ToolCall,
    };

    Kind kind{Kind::Text};
    std::size_t content_index{};
    std::string partial_arguments{};
};

[[nodiscard]] const support::JsonValue* member(const JsonObject& value, std::string_view name) {
    const auto found = value.find(std::string{name});
    return found == value.end() ? nullptr : &found->second;
}

[[nodiscard]] const JsonObject* object_member(const JsonObject& value, std::string_view name) {
    const auto* found = member(value, name);
    return found ? found->get_if<JsonObject>() : nullptr;
}

[[nodiscard]] const JsonArray* array_member(const JsonObject& value, std::string_view name) {
    const auto* found = member(value, name);
    return found ? found->get_if<JsonArray>() : nullptr;
}

[[nodiscard]] std::optional<std::string_view> string_member(const JsonObject& value, std::string_view name) {
    const auto* found = member(value, name);
    const auto* text = found ? found->get_if<std::string>() : nullptr;
    return text ? std::optional<std::string_view>{*text} : std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> integer_member(const JsonObject& value, std::string_view name) {
    const auto* found = member(value, name);
    const auto* number = found ? found->get_if<double>() : nullptr;
    if (!number || !std::isfinite(*number) || *number < 0 ||
            *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

[[nodiscard]] std::optional<std::size_t> output_index(const JsonObject& event) {
    const auto value = integer_member(event, "output_index");
    return value ? std::optional<std::size_t>{static_cast<std::size_t>(*value)} : std::nullopt;
}

void finalize_tool_arguments(ToolCallContent& tool) {
    // Streaming-tolerant argument parsing matching pi's `parseStreamingJson`
    // (partial-json semantics), shared with the Anthropic adapter.
    tool.arguments = parse_streaming_json(tool.raw_arguments);
    tool.arguments_valid = true;
    tool.argument_error = std::nullopt;
}

[[nodiscard]] std::string joined_item_text(const JsonObject& item, std::string_view array_name) {
    const auto* entries = array_member(item, array_name);
    if (!entries) {
        return {};
    }
    std::string result;
    for (const auto& entry : *entries) {
        const auto* entry_object = entry.get_if<JsonObject>();
        if (!entry_object) {
            continue;
        }
        const auto text = string_member(*entry_object, "text");
        const auto refusal = string_member(*entry_object, "refusal");
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
    const auto* entries = array_member(item, "content");
    if (!entries) {
        return {};
    }
    std::string result;
    for (const auto& entry : *entries) {
        const auto* entry_object = entry.get_if<JsonObject>();
        if (!entry_object) {
            continue;
        }
        if (const auto text = string_member(*entry_object, "text")) {
            result += *text;
        } else if (const auto refusal = string_member(*entry_object, "refusal")) {
            result += *refusal;
        }
    }
    return result;
}

/// Text signature v1: {"id": <item-id>, "v": 1, "phase":
/// "commentary"|"final_answer"}.
[[nodiscard]] support::Expected<std::string> text_signature(const JsonObject& item) {
    support::JsonValue::object_t signature{
            {"id", std::string{string_member(item, "id").value_or("")}},
            {"v", 1},
    };
    if (const auto phase = string_member(item, "phase"); phase == "commentary" || phase == "final_answer") {
        signature.emplace("phase", std::string{*phase});
    }
    return support::write_json(support::JsonValue{std::move(signature)});
}

void apply_message_phase_stop_reason(AssistantMessage& assistant, const JsonObject& item) {
    // pi's `applyMessagePhaseStopReason` (openai-responses-shared.ts): a
    // message output item whose `phase` is `final_answer` flips the running
    // partial from `pending` to `stop` before the item is surfaced.
    if (const auto type = string_member(item, "type"); type && *type == "message") {
        if (const auto phase = string_member(item, "phase"); phase && *phase == "final_answer") {
            assistant.stop_reason = AssistantStopReason::Stop;
        }
    }
}

[[nodiscard]] support::ExpectedVoid create_slot(std::size_t index,
        const JsonObject& item,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    if (slots.contains(index)) {
        return {};
    }
    const auto type = string_member(item, "type");
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
        auto arguments = std::string{string_member(item, "arguments").value_or("")};
        assistant.content.emplace_back(ToolCallContent{
                .id = std::string{string_member(item, "call_id").value_or("")} + "|" +
                      std::string{string_member(item, "id").value_or("")},
                .name = std::string{string_member(item, "name").value_or("")},
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
    return {};
}

[[nodiscard]] support::ExpectedVoid append_delta(const JsonObject& event,
        std::string_view type,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto index = output_index(event);
    const auto delta = string_member(event, "delta");
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
    const auto arguments = string_member(event, "arguments");
    if (!index || !arguments) {
        return {};
    }
    const auto found = slots.find(*index);
    if (found == slots.end() || found->second.kind != Slot::Kind::ToolCall) {
        return {};
    }
    auto& slot = found->second;
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
    const auto* item = object_member(event, "item");
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
    const auto type = string_member(*item, "type");
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
        block.id = std::string{string_member(*item, "call_id").value_or("")} + "|" +
                   std::string{string_member(*item, "id").value_or("")};
        block.name = std::string{string_member(*item, "name").value_or("")};
        block.raw_arguments = std::string{string_member(*item, "arguments").value_or(slot.partial_arguments)};
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

[[nodiscard]] ResponsesProviderError provider_error(const JsonObject& event) {
    const auto* nested = object_member(event, "error");
    auto code = string_member(event, "code");
    if (!code && nested) {
        code = string_member(*nested, "code");
    }
    auto message = string_member(event, "message");
    if (!message && nested) {
        message = string_member(*nested, "message");
    }
    return ResponsesProviderError{
            .code = code ? std::optional<std::string>{std::string{*code}} : std::nullopt,
            .message = message ? std::optional<std::string>{std::string{*message}} : std::nullopt,
    };
}

void apply_deepseek_usage(const Model& model, const JsonObject& response, AssistantMessage& assistant) {
    const auto* usage = object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = object_member(*usage, "input_tokens_details");
    const auto* output_details = object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_deepseek_usage(model,
            integer_member(*usage, "input_tokens").value_or(0),
            integer_member(*usage, "output_tokens").value_or(0),
            input_details ? integer_member(*input_details, "cached_tokens").value_or(0) : 0,
            output_details ? integer_member(*output_details, "reasoning_tokens") : std::nullopt);
    if (const auto total = integer_member(*usage, "total_tokens")) {
        assistant.usage.total_tokens = *total;
    }
}

void apply_codex_usage(const Model& model, const JsonObject& response, AssistantMessage& assistant) {
    const auto* usage = object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = object_member(*usage, "input_tokens_details");
    const auto* output_details = object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_responses_usage(model,
            ResponsesUsageFields{
                    .input_tokens = integer_member(*usage, "input_tokens").value_or(0),
                    .output_tokens = integer_member(*usage, "output_tokens").value_or(0),
                    .cached_tokens = input_details ? integer_member(*input_details, "cached_tokens").value_or(0) : 0,
                    .cache_write_tokens =
                            input_details ? integer_member(*input_details, "cache_write_tokens").value_or(0) : 0,
                    .reasoning_tokens =
                            output_details ? integer_member(*output_details, "reasoning_tokens") : std::nullopt,
                    .total_tokens = integer_member(*usage, "total_tokens").value_or(0),
            });
}

[[nodiscard]] support::Expected<ResponsesProcessOutcome> finalize_response(const Model& model,
        ResponsesDialect dialect,
        const JsonObject& event,
        std::string_view event_type,
        AssistantMessage& assistant,
        bool& saw_terminal) {
    // pi's `processResponsesStream` records that a terminal response event
    // arrived before the adapter's final integrity check.
    saw_terminal = true;
    const auto* response = object_member(event, "response");
    if (!response) {
        if (dialect == ResponsesDialect::Codex) {
            assistant.stop_reason = AssistantStopReason::Stop;
            return ResponsesProcessOutcome{.terminal = true};
        }
        return std::unexpected(support::make_error(
                support::ErrorCode::Stream, "OpenAI Responses terminal event omitted response data"));
    }
    if (const auto id = string_member(*response, "id"); id && !id->empty()) {
        assistant.response_id = std::string{*id};
    }
    if (const auto model_id = string_member(*response, "model"); model_id && *model_id != model.id) {
        assistant.response_model = std::string{*model_id};
    }
    if (dialect == ResponsesDialect::DeepSeek) {
        // pi's `finalizeResponse` records the raw wire status for every
        // terminal Responses event, including failures.
        if (const auto status = string_member(*response, "status"); status && !status->empty()) {
            assistant.raw_stop_reason = *status;
        }
        apply_deepseek_usage(model, *response, assistant);
        if (event_type == "response.failed") {
            return ResponsesProcessOutcome{
                    .terminal = true,
                    .provider_error = provider_error(*response),
            };
        }
    } else {
        apply_codex_usage(model, *response, assistant);
    }

    const auto status = string_member(*response, "status").value_or(event_type == "response.done" ? "done" : "");
    std::string_view normalized = status;
    if (dialect == ResponsesDialect::Codex) {
        normalized = "done";
        if (status == "completed" || status == "incomplete" || status == "failed" || status == "cancelled" ||
                status == "queued" || status == "in_progress") {
            if (status == "completed" || status == "incomplete" || status == "failed" || status == "cancelled") {
                normalized = status;
            }
            // pi's Codex mapper records only recognized statuses as raw stop
            // reasons; queued/in_progress normalize to stop through `done`.
            assistant.raw_stop_reason = status;
        }
    }
    auto termination = map_responses_termination(normalized,
            std::ranges::any_of(assistant.content,
                    [](const AssistantContent& block) { return std::holds_alternative<ToolCallContent>(block); }));
    if (!termination) {
        return std::unexpected(termination.error());
    }
    assistant.stop_reason = termination->reason;
    assistant.error_message = termination->error_message;
    return ResponsesProcessOutcome{.terminal = true};
}

[[nodiscard]] support::Expected<ResponsesProcessOutcome> process_event(const Model& model,
        ResponsesDialect dialect,
        ResponsesDelivery delivery,
        JsonObject event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        bool& saw_terminal) {
    const auto type = string_member(event, "type");
    if (!type) {
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.created") {
        if (const auto* response = object_member(event, "response")) {
            if (const auto id = string_member(*response, "id"); id && !id->empty()) {
                assistant.response_id = std::string{*id};
            }
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.output_item.added") {
        const auto index = output_index(event);
        const auto* item = object_member(event, "item");
        if (index && item) {
            if (auto created = create_slot(*index, *item, slots, assistant, sink); !created) {
                return std::unexpected(created.error());
            }
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.reasoning_summary_text.delta" || *type == "response.reasoning_text.delta" ||
            *type == "response.output_text.delta" || *type == "response.refusal.delta" ||
            *type == "response.function_call_arguments.delta") {
        if (auto processed = append_delta(event, *type, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.reasoning_summary_part.done") {
        if (auto processed = append_reasoning_separator(event, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.function_call_arguments.done") {
        if (auto processed = finish_argument_stream(event, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.output_item.done") {
        if (auto processed = finish_output_item(event, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.completed" || *type == "response.done" || *type == "response.incomplete") {
        return finalize_response(model, dialect, event, *type, assistant, saw_terminal);
    }
    if (*type == "response.failed" || *type == "error") {
        if (dialect == ResponsesDialect::DeepSeek && *type == "response.failed") {
            return finalize_response(model, dialect, event, *type, assistant, saw_terminal);
        }
        auto failure = provider_error(event);
        if (*type == "response.failed" && delivery == ResponsesDelivery::WebSocket) {
            // Codex WebSocket failures carry retryable codes under response.error;
            // SSE retains its historical top-level error extraction.
            if (const auto* response = object_member(event, "response")) {
                auto response_failure = provider_error(*response);
                if (!failure.code) {
                    failure.code = std::move(response_failure.code);
                }
                if (!failure.message) {
                    failure.message = std::move(response_failure.message);
                }
            }
        }
        return ResponsesProcessOutcome{
                .terminal = false,
                .provider_error = std::move(failure),
        };
    }
    return ResponsesProcessOutcome{};
}

} // namespace

struct ResponsesEventProcessor::Impl {
    Impl(ResponsesDialect configured_dialect, ResponsesDelivery configured_delivery, Model configured_model)
        : dialect(configured_dialect), delivery(configured_delivery), model(std::move(configured_model)) {}

    ResponsesDialect dialect;
    ResponsesDelivery delivery;
    Model model;
    std::map<std::size_t, Slot> slots;
    bool saw_terminal{false};
};

ResponsesEventProcessor::ResponsesEventProcessor(ResponsesDialect dialect, ResponsesDelivery delivery, Model model)
    : impl_(std::make_unique<Impl>(dialect, delivery, std::move(model))) {}

ResponsesEventProcessor::ResponsesEventProcessor(ResponsesEventProcessor&&) noexcept = default;
ResponsesEventProcessor& ResponsesEventProcessor::operator=(ResponsesEventProcessor&&) noexcept = default;
ResponsesEventProcessor::~ResponsesEventProcessor() = default;

support::Expected<ResponsesProcessOutcome> ResponsesEventProcessor::process(
        JsonObject event, AssistantMessage& assistant, AssistantEventSink& sink) {
    return process_event(impl_->model,
            impl_->dialect,
            impl_->delivery,
            std::move(event),
            impl_->slots,
            assistant,
            sink,
            impl_->saw_terminal);
}

support::ExpectedVoid ResponsesEventProcessor::finish([[maybe_unused]] AssistantMessage&) {
    if (!impl_->saw_terminal) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Stream, "OpenAI Responses stream ended before a terminal response event"));
    }
    return {};
}

} // namespace cch::ai::api
