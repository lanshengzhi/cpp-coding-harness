#include "AnthropicMessagesAdapter.hpp"

#include "MessageConversion.hpp"
#include "PartialJson.hpp"
#include "Termination.hpp"
#include "UsageNormalization.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

using JsonObject = support::JsonValue::object_t;

struct BlockSlot {
    enum class Kind { Thinking, Text, ToolCall };

    Kind kind{Kind::Text};
    std::size_t content_index{};
    std::string partial_arguments;
};

[[nodiscard]] TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] const JsonObject* object(const support::JsonValue& value) {
    return value.get_if<JsonObject>();
}

[[nodiscard]] const support::JsonValue* member(
    const JsonObject& value,
    std::string_view name) {
    const auto found = value.find(std::string{name});
    return found == value.end() ? nullptr : &found->second;
}

[[nodiscard]] const JsonObject* object_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    return found ? found->get_if<JsonObject>() : nullptr;
}

[[nodiscard]] std::optional<std::string_view> string_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    const auto* text = found ? found->get_if<std::string>() : nullptr;
    return text ? std::optional<std::string_view>{*text} : std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> integer_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    const auto* number = found ? found->get_if<double>() : nullptr;
    if (!number || !std::isfinite(*number) || *number < 0 ||
        *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

[[nodiscard]] std::optional<std::size_t> block_index(const JsonObject& event) {
    const auto index = integer_member(event, "index");
    return index ? std::optional<std::size_t>{static_cast<std::size_t>(*index)}
                 : std::nullopt;
}

[[nodiscard]] bool header_name_equal(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char left_character, char right_character) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        };
        return lower(left_character) == lower(right_character);
    });
}

template <typename Headers>
void set_header(Headers& headers, std::string name, std::string value) {
    std::erase_if(headers, [&name](const auto& header) {
        return header_name_equal(header.first, name);
    });
    headers.emplace(std::move(name), std::move(value));
}

template <typename Headers>
[[nodiscard]] bool has_header(const Headers& headers, std::string_view name) {
    return std::ranges::any_of(headers, [name](const auto& header) {
        return header_name_equal(header.first, name) && !header.second.empty();
    });
}

[[nodiscard]] bool header_deleted(
    const ProviderStreamOptions& options,
    std::string_view name) {
    return std::ranges::any_of(options.deleted_headers, [name](const auto& header) {
        return header_name_equal(header, name);
    });
}

[[nodiscard]] std::string messages_url(std::string_view base_url) {
    std::string result{base_url};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result + "/v1/messages";
}

[[nodiscard]] support::Expected<providers::StreamRequest> build_stream_request(
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
    auto payload = build_adapter_payload(
        AdapterKind::AnthropicMessages, model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = support::write_json(*payload);
    if (!body) {
        return std::unexpected(body.error());
    }

    providers::StreamRequest request;
    request.url = messages_url(model.base_url);
    request.timeout = std::chrono::milliseconds{options.timeout_ms.value_or(30000)};
    request.stop_token = options.stop_token;
    if (model.headers) {
        for (const auto& [name, value] : *model.headers) {
            if (!header_deleted(options, name)) {
                set_header(request.headers, name, value);
            }
        }
    }
    for (const auto& [name, value] : options.auth.headers) {
        set_header(request.headers, name, value);
    }
    if (options.auth.api_key &&
        !has_header(request.headers, "authorization") &&
        !has_header(request.headers, "x-api-key") &&
        !header_deleted(options, "x-api-key")) {
        set_header(request.headers, "x-api-key", *options.auth.api_key);
    }
    if (!has_header(request.headers, "anthropic-version") &&
        !header_deleted(options, "anthropic-version")) {
        set_header(request.headers, "anthropic-version", "2023-06-01");
    }
    if (!has_header(request.headers, "anthropic-dangerous-direct-browser-access") &&
        !header_deleted(options, "anthropic-dangerous-direct-browser-access")) {
        set_header(
            request.headers,
            "anthropic-dangerous-direct-browser-access",
            "true");
    }
    if (!has_header(request.headers, "content-type") &&
        !header_deleted(options, "content-type")) {
        set_header(request.headers, "Content-Type", "application/json");
    }
    if (!has_header(request.headers, "accept") &&
        !header_deleted(options, "accept")) {
        set_header(request.headers, "Accept", "application/json");
    }
    request.body = std::move(*body);
    return request;
}

[[nodiscard]] support::Error stream_error(std::string message, std::string detail = {}) {
    return support::make_error(
        support::ErrorCode::Stream,
        providers::bounded_provider_error_detail(std::move(message)),
        providers::bounded_provider_error_detail(std::move(detail)));
}

[[nodiscard]] support::Expected<support::JsonValue> parse_event_json(
    const providers::SseEvent& event) {
    if (auto parsed = support::read_json(event.data)) {
        return std::move(*parsed);
    }
    const auto repaired = repair_json_strings(event.data);
    if (auto parsed = support::read_json(repaired)) {
        return std::move(*parsed);
    }
    return std::unexpected(stream_error(
        "Could not parse Anthropic SSE event " + event.event,
        event.data));
}

[[nodiscard]] AnthropicUsageUpdate usage_update(const JsonObject& usage) {
    // pi records `cacheWrite1h` on every message_start, defaulting to 0 when
    // the provider omits `cache_creation` (anthropic-messages.ts
    // `cache_creation?.ephemeral_1h_input_tokens || 0`).
    std::optional<std::int64_t> cache_write_1h = 0;
    if (const auto* cache_creation = object_member(usage, "cache_creation")) {
        cache_write_1h =
            integer_member(*cache_creation, "ephemeral_1h_input_tokens");
        if (!cache_write_1h) {
            cache_write_1h = 0;
        }
    }
    AnthropicUsageUpdate update{
        .input = integer_member(usage, "input_tokens"),
        .output = integer_member(usage, "output_tokens"),
        .cache_read = integer_member(usage, "cache_read_input_tokens"),
        .cache_write = integer_member(usage, "cache_creation_input_tokens"),
        .cache_write_1h = cache_write_1h,
        .reasoning = std::nullopt,
    };
    if (const auto* output_details = object_member(usage, "output_tokens_details")) {
        update.reasoning = integer_member(*output_details, "thinking_tokens");
    }
    return update;
}

[[nodiscard]] support::ExpectedVoid start_content_block(
    const JsonObject& event,
    std::map<std::size_t, BlockSlot>& slots,
    AssistantMessage& assistant,
    AssistantEventSink& sink) {
    const auto provider_index = block_index(event);
    const auto* content = object_member(event, "content_block");
    if (!provider_index || !content || slots.contains(*provider_index)) {
        return {};
    }
    const auto type = string_member(*content, "type");
    if (!type) {
        return {};
    }
    const auto content_index = assistant.content.size();
    if (*type == "text") {
        assistant.content.emplace_back(TextContent{
            .text = std::string{string_member(*content, "text").value_or("")},
            .text_signature = std::nullopt,
        });
        slots.emplace(*provider_index, BlockSlot{
            .kind = BlockSlot::Kind::Text,
            .content_index = content_index,
            .partial_arguments = {},
        });
        return providers::emit(sink, TextStartEvent{
            .content_index = content_index,
            .partial = assistant,
        });
    }
    if (*type == "thinking") {
        assistant.content.emplace_back(ThinkingContent{
            .thinking = std::string{string_member(*content, "thinking").value_or("")},
            .thinking_signature = std::string{
                string_member(*content, "signature").value_or("")},
            .redacted = false,
        });
        slots.emplace(*provider_index, BlockSlot{
            .kind = BlockSlot::Kind::Thinking,
            .content_index = content_index,
            .partial_arguments = {},
        });
        return providers::emit(sink, ThinkingStartEvent{
            .content_index = content_index,
            .partial = assistant,
        });
    }
    if (*type == "redacted_thinking") {
        assistant.content.emplace_back(ThinkingContent{
            .thinking = "[Reasoning redacted]",
            .thinking_signature = std::string{
                string_member(*content, "data").value_or("")},
            .redacted = true,
        });
        slots.emplace(*provider_index, BlockSlot{
            .kind = BlockSlot::Kind::Thinking,
            .content_index = content_index,
            .partial_arguments = {},
        });
        return providers::emit(sink, ThinkingStartEvent{
            .content_index = content_index,
            .partial = assistant,
        });
    }
    if (*type == "tool_use") {
        const auto* input = member(*content, "input");
        assistant.content.emplace_back(ToolCallContent{
            .id = std::string{string_member(*content, "id").value_or("")},
            .name = std::string{string_member(*content, "name").value_or("")},
            .arguments = input ? std::optional<support::JsonValue>{*input}
                               : std::optional<support::JsonValue>{support::JsonValue::object_t{}},
            .raw_arguments = {},
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
        });
        slots.emplace(*provider_index, BlockSlot{
            .kind = BlockSlot::Kind::ToolCall,
            .content_index = content_index,
            .partial_arguments = {},
        });
        return providers::emit(sink, ToolCallStartEvent{
            .content_index = content_index,
            .partial = assistant,
        });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid append_content_delta(
    const JsonObject& event,
    std::map<std::size_t, BlockSlot>& slots,
    AssistantMessage& assistant,
    AssistantEventSink& sink) {
    const auto provider_index = block_index(event);
    const auto* delta = object_member(event, "delta");
    if (!provider_index || !delta) {
        return {};
    }
    const auto found = slots.find(*provider_index);
    const auto type = string_member(*delta, "type");
    if (found == slots.end() || !type) {
        return {};
    }
    auto& slot = found->second;
    if (*type == "text_delta" && slot.kind == BlockSlot::Kind::Text) {
        const auto text = string_member(*delta, "text").value_or("");
        auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        block.text += text;
        return providers::emit(sink, TextDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{text},
            .partial = assistant,
        });
    }
    if (*type == "thinking_delta" && slot.kind == BlockSlot::Kind::Thinking) {
        const auto thinking = string_member(*delta, "thinking").value_or("");
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        block.thinking += thinking;
        return providers::emit(sink, ThinkingDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{thinking},
            .partial = assistant,
        });
    }
    if (*type == "signature_delta" && slot.kind == BlockSlot::Kind::Thinking) {
        const auto signature = string_member(*delta, "signature").value_or("");
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        block.thinking_signature = block.thinking_signature.value_or("") +
                                   std::string{signature};
        return {};
    }
    if (*type == "input_json_delta" && slot.kind == BlockSlot::Kind::ToolCall) {
        const auto partial = string_member(*delta, "partial_json").value_or("");
        slot.partial_arguments += partial;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        block.raw_arguments = slot.partial_arguments;
        block.arguments = parse_streaming_json(block.raw_arguments);
        block.arguments_valid = true;
        block.argument_error = std::nullopt;
        return providers::emit(sink, ToolCallDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{partial},
            .partial = assistant,
        });
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid stop_content_block(
    const JsonObject& event,
    std::map<std::size_t, BlockSlot>& slots,
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
    if (slot.kind == BlockSlot::Kind::Text) {
        const auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        return providers::emit(sink, TextEndEvent{
            .content_index = slot.content_index,
            .content = block.text,
            .partial = assistant,
        });
    }
    if (slot.kind == BlockSlot::Kind::Thinking) {
        const auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        return providers::emit(sink, ThinkingEndEvent{
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
    return providers::emit(sink, ToolCallEndEvent{
        .content_index = slot.content_index,
        .tool_call = block,
        .partial = assistant,
    });
}

[[nodiscard]] support::ExpectedVoid process_json_event(
    const Model& model,
    const JsonObject& event,
    AssistantMessage& assistant,
    std::map<std::size_t, BlockSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_message_start,
    bool& saw_message_stop,
    std::optional<TerminationResult>& termination) {
    const auto type = string_member(event, "type");
    if (!type) {
        return {};
    }
    if (*type == "message_start") {
        saw_message_start = true;
        const auto* message = object_member(event, "message");
        if (!message) {
            return {};
        }
        if (const auto id = string_member(*message, "id"); id && !id->empty()) {
            assistant.response_id = std::string{*id};
        }
        if (const auto* usage = object_member(*message, "usage")) {
            apply_anthropic_usage_start(model, assistant.usage, usage_update(*usage));
        }
        return {};
    }
    if (*type == "content_block_start") {
        return start_content_block(event, slots, assistant, sink);
    }
    if (*type == "content_block_delta") {
        return append_content_delta(event, slots, assistant, sink);
    }
    if (*type == "content_block_stop") {
        return stop_content_block(event, slots, assistant, sink);
    }
    if (*type == "message_delta") {
        const auto* delta = object_member(event, "delta");
        if (delta) {
            if (const auto reason = string_member(*delta, "stop_reason")) {
                assistant.raw_stop_reason = std::string{*reason};
                std::optional<std::string_view> explanation;
                if (const auto* details = object_member(*delta, "stop_details")) {
                    explanation = string_member(*details, "explanation");
                }
                auto mapped = map_anthropic_termination(*reason, explanation);
                if (!mapped) {
                    return std::unexpected(mapped.error());
                }
                termination = std::move(*mapped);
                assistant.stop_reason = termination->reason;
                assistant.error_message = termination->error_message;
            }
        }
        if (const auto* usage = object_member(event, "usage")) {
            apply_anthropic_usage_delta(model, assistant.usage, usage_update(*usage));
        }
        return {};
    }
    if (*type == "message_stop") {
        saw_message_stop = true;
    }
    return {};
}

[[nodiscard]] bool known_anthropic_event(std::string_view event) {
    return event == "message_start" || event == "message_delta" ||
           event == "message_stop" || event == "content_block_start" ||
           event == "content_block_delta" || event == "content_block_stop";
}

[[nodiscard]] support::ExpectedVoid process_sse_event(
    const providers::SseEvent& event,
    const Model& model,
    AssistantMessage& assistant,
    std::map<std::size_t, BlockSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_message_start,
    bool& saw_message_stop,
    std::optional<TerminationResult>& termination) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream_error(event.data));
    }
    if (!known_anthropic_event(event.event)) {
        return {};
    }
    auto parsed = parse_event_json(event);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto* event_object = object(*parsed);
    if (!event_object) {
        return std::unexpected(stream_error(
            "Could not parse Anthropic SSE event " + event.event,
            "event data must be a JSON object"));
    }
    return process_json_event(
        model,
        *event_object,
        assistant,
        slots,
        sink,
        saw_message_start,
        saw_message_stop,
        termination);
}

} // namespace

AnthropicMessagesAdapter::AnthropicMessagesAdapter(
    std::shared_ptr<providers::StreamTransport> transport)
    : transport_(std::move(transport)) {}

AnthropicMessagesAdapter::AnthropicMessagesAdapter(AnthropicMessagesAdapter&&) noexcept = default;
AnthropicMessagesAdapter& AnthropicMessagesAdapter::operator=(AnthropicMessagesAdapter&&) noexcept = default;
AnthropicMessagesAdapter::~AnthropicMessagesAdapter() = default;

boost::asio::awaitable<support::Expected<AssistantMessage>> AnthropicMessagesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!transport_) {
        co_return std::unexpected(stream_error(
            "Anthropic Messages adapter requires a stream transport"));
    }
    if (model.api != "anthropic-messages") {
        co_return std::unexpected(stream_error(
            "Anthropic Messages adapter received the wrong Model API"));
    }
    if (model.base_url.empty()) {
        co_return std::unexpected(stream_error(
            "Anthropic Messages Model base URL is required"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if ((!options.auth.api_key || options.auth.api_key->empty()) &&
        !has_header(options.auth.headers, "authorization") &&
        !has_header(options.auth.headers, "x-api-key") &&
        !has_header(options.auth.headers, "cf-aig-authorization")) {
        co_return std::unexpected(stream_error(
            "No API key for provider: " + model.provider));
    }

    CCH_TRY(request, build_stream_request(model, context, options));

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = current_timestamp_ms();

    struct AttemptState {
        std::map<std::size_t, BlockSlot> slots{};
        bool saw_message_start{false};
        bool saw_message_stop{false};
        std::optional<TerminationResult> termination{std::nullopt};
    };

    auto attempt_state = std::make_shared<AttemptState>();

    auto attempt_hook = [attempt_state, &model]() -> support::Expected<providers::SseEventHook> {
        *attempt_state = AttemptState{};
        return [attempt_state, &model](
            const providers::SseEvent& event,
            AssistantMessage& assistant,
            AssistantEventSink& sink) -> support::ExpectedVoid {
            return process_sse_event(
                event,
                model,
                assistant,
                attempt_state->slots,
                sink,
                attempt_state->saw_message_start,
                attempt_state->saw_message_stop,
                attempt_state->termination);
        };
    };

    auto finalize_hook = [attempt_state](AssistantMessage& assistant) -> support::ExpectedVoid {
        if (!attempt_state->saw_message_stop) {
            return std::unexpected(stream_error(attempt_state->saw_message_start
                ? "Anthropic stream ended before message_stop"
                : "Anthropic stream ended without message_stop"));
        }
        if (assistant.stop_reason == AssistantStopReason::Pending) {
            return std::unexpected(stream_error(
                "Anthropic stream ended without a stop reason"));
        }
        if (attempt_state->termination && attempt_state->termination->reason == AssistantStopReason::Error) {
            assistant.stop_reason = AssistantStopReason::Error;
            return std::unexpected(stream_error(
                attempt_state->termination->error_message.value_or("Anthropic Messages request failed")));
        }
        if (attempt_state->termination) {
            assistant.stop_reason = attempt_state->termination->reason;
            assistant.error_message = std::nullopt;
        }
        return {};
    };

    co_return co_await providers::execute_sse_stream(providers::SseStreamExecutionOptions{
        .protocol_name = "Anthropic Messages",
        .request = request,
        .transport = *transport_,
        .options = options,
        .initial_assistant = std::move(assistant),
        .sink = std::move(sink),
        .attempt_hook = std::move(attempt_hook),
        .finalize_hook = std::move(finalize_hook),
    });
}

} // namespace cch::ai::api
