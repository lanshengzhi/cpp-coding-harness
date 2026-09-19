#include "AnthropicEvents.hpp"

#include "PartialJson.hpp"
#include "ai/JsonAccess.hpp"
#include "ai/Timestamps.hpp"
#include "ai/api/UsageNormalization.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
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

using JsonObject = support::JsonValue::object_t;

[[nodiscard]] std::optional<std::size_t> block_index(const JsonObject& event) {
    const auto index = json_integer_member(event, "index");
    return index ? std::optional<std::size_t>{static_cast<std::size_t>(*index)} : std::nullopt;
}

[[nodiscard]] support::Expected<support::JsonValue> parse_event_json(const providers::SseEvent& event) {
    if (auto parsed = support::read_json(event.data)) {
        return std::move(*parsed);
    }
    const auto repaired = repair_json_strings(event.data);
    if (auto parsed = support::read_json(repaired)) {
        return std::move(*parsed);
    }
    return std::unexpected(
            providers::make_stream_error("Could not parse Anthropic SSE event " + event.event, event.data));
}

[[nodiscard]] AnthropicUsageUpdate usage_update(const JsonObject& usage) {
    // pi records `cacheWrite1h` on every message_start, defaulting to 0 when
    // the provider omits `cache_creation` (anthropic-messages.ts
    // `cache_creation?.ephemeral_1h_input_tokens || 0`).
    std::optional<std::int64_t> cache_write_1h = 0;
    if (const auto* cache_creation = json_object_member(usage, "cache_creation")) {
        cache_write_1h = json_integer_member(*cache_creation, "ephemeral_1h_input_tokens");
        if (!cache_write_1h) {
            cache_write_1h = 0;
        }
    }
    AnthropicUsageUpdate update{
            .input = json_integer_member(usage, "input_tokens"),
            .output = json_integer_member(usage, "output_tokens"),
            .cache_read = json_integer_member(usage, "cache_read_input_tokens"),
            .cache_write = json_integer_member(usage, "cache_creation_input_tokens"),
            .cache_write_1h = cache_write_1h,
            .reasoning = std::nullopt,
    };
    if (const auto* output_details = json_object_member(usage, "output_tokens_details")) {
        update.reasoning = json_integer_member(*output_details, "thinking_tokens");
    }
    return update;
}

[[nodiscard]] support::ExpectedVoid start_content_block(const JsonObject& event,
        std::map<std::size_t, AnthropicBlockSlot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto provider_index = block_index(event);
    const auto* content = json_object_member(event, "content_block");
    if (!provider_index || !content || slots.contains(*provider_index)) {
        return {};
    }
    const auto type = json_string_member(*content, "type");
    if (!type) {
        return {};
    }
    const auto content_index = assistant.content.size();
    if (*type == "text") {
        assistant.content.emplace_back(TextContent{
                .text = std::string{json_string_member(*content, "text").value_or("")},
                .text_signature = std::nullopt,
        });
        slots.emplace(*provider_index,
                AnthropicBlockSlot{
                        .kind = AnthropicBlockSlot::Kind::Text,
                        .content_index = content_index,
                        .partial_arguments = {},
                });
        return providers::emit(sink,
                TextStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    if (*type == "thinking") {
        assistant.content.emplace_back(ThinkingContent{
                .thinking = std::string{json_string_member(*content, "thinking").value_or("")},
                .thinking_signature = std::string{json_string_member(*content, "signature").value_or("")},
                .redacted = false,
        });
        slots.emplace(*provider_index,
                AnthropicBlockSlot{
                        .kind = AnthropicBlockSlot::Kind::Thinking,
                        .content_index = content_index,
                        .partial_arguments = {},
                });
        return providers::emit(sink,
                ThinkingStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    if (*type == "redacted_thinking") {
        assistant.content.emplace_back(ThinkingContent{
                .thinking = "[Reasoning redacted]",
                .thinking_signature = std::string{json_string_member(*content, "data").value_or("")},
                .redacted = true,
        });
        slots.emplace(*provider_index,
                AnthropicBlockSlot{
                        .kind = AnthropicBlockSlot::Kind::Thinking,
                        .content_index = content_index,
                        .partial_arguments = {},
                });
        return providers::emit(sink,
                ThinkingStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    if (*type == "tool_use") {
        const auto* input = json_member(*content, "input");
        assistant.content.emplace_back(ToolCallContent{
                .id = std::string{json_string_member(*content, "id").value_or("")},
                .name = std::string{json_string_member(*content, "name").value_or("")},
                .arguments = input ? std::optional<support::JsonValue>{*input}
                                   : std::optional<support::JsonValue>{support::JsonValue::object_t{}},
                .raw_arguments = {},
                .thought_signature = std::nullopt,
                .arguments_valid = true,
                .argument_error = std::nullopt,
        });
        slots.emplace(*provider_index,
                AnthropicBlockSlot{
                        .kind = AnthropicBlockSlot::Kind::ToolCall,
                        .content_index = content_index,
                        .partial_arguments = {},
                });
        return providers::emit(sink,
                ToolCallStartEvent{
                        .content_index = content_index,
                        .partial = assistant,
                });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid append_content_delta(const JsonObject& event,
        std::map<std::size_t, AnthropicBlockSlot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto provider_index = block_index(event);
    const auto* delta = json_object_member(event, "delta");
    if (!provider_index || !delta) {
        return {};
    }
    const auto found = slots.find(*provider_index);
    const auto type = json_string_member(*delta, "type");
    if (found == slots.end() || !type) {
        return {};
    }
    auto& slot = found->second;
    if (*type == "text_delta" && slot.kind == AnthropicBlockSlot::Kind::Text) {
        const auto text = json_string_member(*delta, "text").value_or("");
        auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        block.text += text;
        return providers::emit(sink,
                TextDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{text},
                        .partial = assistant,
                });
    }
    if (*type == "thinking_delta" && slot.kind == AnthropicBlockSlot::Kind::Thinking) {
        const auto thinking = json_string_member(*delta, "thinking").value_or("");
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        block.thinking += thinking;
        return providers::emit(sink,
                ThinkingDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{thinking},
                        .partial = assistant,
                });
    }
    if (*type == "signature_delta" && slot.kind == AnthropicBlockSlot::Kind::Thinking) {
        const auto signature = json_string_member(*delta, "signature").value_or("");
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        block.thinking_signature = block.thinking_signature.value_or("") + std::string{signature};
        return {};
    }
    if (*type == "input_json_delta" && slot.kind == AnthropicBlockSlot::Kind::ToolCall) {
        const auto partial = json_string_member(*delta, "partial_json").value_or("");
        slot.partial_arguments += partial;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        block.raw_arguments = slot.partial_arguments;
        block.arguments = parse_streaming_json(block.raw_arguments);
        block.arguments_valid = true;
        block.argument_error = std::nullopt;
        return providers::emit(sink,
                ToolCallDeltaEvent{
                        .content_index = slot.content_index,
                        .delta = std::string{partial},
                        .partial = assistant,
                });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid stop_content_block(const JsonObject& event,
        std::map<std::size_t, AnthropicBlockSlot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto provider_index = block_index(event);
    if (!provider_index) {
        return {};
    }
    const auto found = slots.find(*provider_index);
    if (found == slots.end()) {
        return {};
    }
    const auto slot = found->second;
    slots.erase(found);
    if (slot.kind == AnthropicBlockSlot::Kind::Text) {
        const auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        return providers::emit(sink,
                TextEndEvent{
                        .content_index = slot.content_index,
                        .content = block.text,
                        .partial = assistant,
                });
    }
    if (slot.kind == AnthropicBlockSlot::Kind::Thinking) {
        const auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        return providers::emit(sink,
                ThinkingEndEvent{
                        .content_index = slot.content_index,
                        .content = block.thinking,
                        .partial = assistant,
                });
    }
    auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
    block.raw_arguments = slot.partial_arguments;
    block.arguments = parse_streaming_json(block.raw_arguments);
    block.arguments_valid = true;
    block.argument_error = std::nullopt;
    return providers::emit(sink,
            ToolCallEndEvent{
                    .content_index = slot.content_index,
                    .tool_call = block,
                    .partial = assistant,
            });
}

/// Block-level events dispatch to handlers with one shared signature; the
/// message-level events keep bespoke bodies below.
using BlockHandler = support::ExpectedVoid (*)(
        const JsonObject&, std::map<std::size_t, AnthropicBlockSlot>&, AssistantMessage&, AssistantEventSink&);

constexpr std::pair<std::string_view, BlockHandler> kBlockHandlers[] = {
        {"content_block_start", &start_content_block},
        {"content_block_delta", &append_content_delta},
        {"content_block_stop", &stop_content_block},
};

constexpr std::string_view kMessageEvents[] = {
        "message_start",
        "message_delta",
        "message_stop",
};

[[nodiscard]] support::ExpectedVoid process_json_event(const Model& model,
        const JsonObject& event,
        AssistantMessage& assistant,
        AnthropicEventState& state,
        AssistantEventSink& sink) {
    const auto type = json_string_member(event, "type");
    if (!type) {
        return {};
    }
    for (const auto& [name, handler] : kBlockHandlers) {
        if (*type == name) {
            return handler(event, state.slots, assistant, sink);
        }
    }
    if (*type == "message_start") {
        state.saw_message_start = true;
        const auto* message = json_object_member(event, "message");
        if (!message) {
            return {};
        }
        if (const auto id = json_string_member(*message, "id"); id && !id->empty()) {
            assistant.response_id = std::string{*id};
        }
        if (const auto* usage = json_object_member(*message, "usage")) {
            apply_anthropic_usage_start(model, assistant.usage, usage_update(*usage));
        }
        return {};
    }
    if (*type == "message_delta") {
        const auto* delta = json_object_member(event, "delta");
        if (delta) {
            if (const auto reason = json_string_member(*delta, "stop_reason")) {
                assistant.raw_stop_reason = std::string{*reason};
                std::optional<std::string_view> explanation;
                if (const auto* details = json_object_member(*delta, "stop_details")) {
                    explanation = json_string_member(*details, "explanation");
                }
                auto mapped = map_anthropic_termination(*reason, explanation);
                if (!mapped) {
                    return std::unexpected(mapped.error());
                }
                state.termination = std::move(*mapped);
                assistant.stop_reason = state.termination->reason;
                assistant.error_message = state.termination->error_message;
            }
        }
        if (const auto* usage = json_object_member(event, "usage")) {
            apply_anthropic_usage_delta(model, assistant.usage, usage_update(*usage));
        }
        return {};
    }
    if (*type == "message_stop") {
        state.saw_message_stop = true;
    }
    return {};
}

[[nodiscard]] bool known_anthropic_event(std::string_view event) {
    return std::ranges::contains(kMessageEvents, event) ||
           std::ranges::any_of(kBlockHandlers, [event](const auto& entry) { return entry.first == event; });
}

} // namespace

support::ExpectedVoid process_anthropic_sse_event(const providers::SseEvent& event,
        const Model& model,
        AssistantMessage& assistant,
        AnthropicEventState& state,
        AssistantEventSink& sink,
        std::optional<InferenceFailure>& inference_failure) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        const auto provider_code = providers::provider_error_code_from_payload(event.data);
        inference_failure = InferenceFailure{
                .kind = provider_code ? providers::inference_failure_kind_from_provider_code(*provider_code)
                                      : InferenceFailureKind::InvalidRequest,
                .output_started = false,
                .suggested_backoff_ms = providers::provider_backoff_hint_ms(event.data, current_timestamp_ms()),
                .provider_code = provider_code,
        };
        return std::unexpected(providers::make_stream_error(event.data));
    }
    if (!known_anthropic_event(event.event)) {
        return {};
    }
    auto parsed = parse_event_json(event);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto* event_object = json_object(*parsed);
    if (!event_object) {
        return std::unexpected(providers::make_stream_error(
                "Could not parse Anthropic SSE event " + event.event, "event data must be a JSON object"));
    }
    return process_json_event(model, *event_object, assistant, state, sink);
}

support::ExpectedVoid finalize_anthropic_stream(AnthropicEventState& state, AssistantMessage& assistant) {
    if (!state.saw_message_stop) {
        return std::unexpected(
                providers::make_stream_error(state.saw_message_start ? "Anthropic stream ended before message_stop"
                                                                     : "Anthropic stream ended without message_stop"));
    }
    if (assistant.stop_reason == AssistantStopReason::Pending) {
        return std::unexpected(providers::make_stream_error("Anthropic stream ended without a stop reason"));
    }
    if (state.termination && state.termination->reason == AssistantStopReason::Error) {
        assistant.stop_reason = AssistantStopReason::Error;
        return std::unexpected(providers::make_stream_error(
                state.termination->error_message.value_or("Anthropic Messages request failed")));
    }
    if (state.termination) {
        assistant.stop_reason = state.termination->reason;
        assistant.error_message = std::nullopt;
    }
    return {};
}

} // namespace cch::ai::api
