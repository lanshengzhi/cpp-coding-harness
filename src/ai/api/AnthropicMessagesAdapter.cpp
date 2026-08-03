#include "AnthropicMessagesAdapter.hpp"

#include "MessageConversion.hpp"
#include "Termination.hpp"
#include "UsageNormalization.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
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

using JsonObject = util::JsonValue::object_t;

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

[[nodiscard]] const JsonObject* object(const util::JsonValue& value) {
    return value.get_if<JsonObject>();
}

[[nodiscard]] const util::JsonValue* member(
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

[[nodiscard]] util::Expected<providers::StreamRequest> build_stream_request(
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
    auto payload = build_adapter_payload(
        AdapterKind::AnthropicMessages, model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = util::write_json(*payload);
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

[[nodiscard]] util::Error stream_error(std::string message, std::string detail = {}) {
    return util::make_error(
        util::ErrorCode::Stream,
        providers::bounded_provider_error_detail(std::move(message)),
        providers::bounded_provider_error_detail(std::move(detail)));
}

[[nodiscard]] util::Error normalize_transport_error(const util::Error& error) {
    if (error.code == util::ErrorCode::Cancelled) {
        return util::make_error(util::ErrorCode::Cancelled, "Request was aborted");
    }
    if (error.code == util::ErrorCode::Unknown) {
        return error;
    }
    return stream_error(
        error.message.empty() ? "Anthropic Messages request failed" : error.message,
        error.detail);
}

[[nodiscard]] std::string repair_json_strings(std::string_view json) {
    std::string repaired;
    repaired.reserve(json.size());
    bool in_string = false;
    for (std::size_t index = 0; index < json.size(); ++index) {
        const auto character = static_cast<unsigned char>(json[index]);
        if (!in_string) {
            repaired.push_back(static_cast<char>(character));
            if (character == '"') {
                in_string = true;
            }
            continue;
        }
        if (character == '"') {
            repaired.push_back('"');
            in_string = false;
            continue;
        }
        if (character == '\\') {
            if (index + 1 >= json.size()) {
                repaired += "\\\\";
                continue;
            }
            const auto next = json[index + 1];
            const bool simple_escape = next == '"' || next == '\\' || next == '/' ||
                                       next == 'b' || next == 'f' || next == 'n' ||
                                       next == 'r' || next == 't';
            if (simple_escape) {
                repaired.push_back('\\');
                repaired.push_back(next);
                ++index;
                continue;
            }
            if (next == 'u' && index + 5 < json.size() &&
                std::ranges::all_of(json.substr(index + 2, 4), [](char digit) {
                    return (digit >= '0' && digit <= '9') ||
                           (digit >= 'a' && digit <= 'f') ||
                           (digit >= 'A' && digit <= 'F');
                })) {
                repaired.append(json.substr(index, 6));
                index += 5;
                continue;
            }
            repaired += "\\\\";
            continue;
        }
        switch (character) {
        case '\b':
            repaired += "\\b";
            break;
        case '\f':
            repaired += "\\f";
            break;
        case '\n':
            repaired += "\\n";
            break;
        case '\r':
            repaired += "\\r";
            break;
        case '\t':
            repaired += "\\t";
            break;
        default:
            if (character < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                repaired += "\\u00";
                repaired.push_back(kHex[(character >> 4U) & 0x0fU]);
                repaired.push_back(kHex[character & 0x0fU]);
            } else {
                repaired.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return repaired;
}

[[nodiscard]] std::string complete_partial_json(std::string json) {
    std::vector<char> closing;
    bool in_string = false;
    bool escaped = false;
    for (const auto character : json) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            closing.push_back('}');
        } else if (character == '[') {
            closing.push_back(']');
        } else if ((character == '}' || character == ']') && !closing.empty()) {
            closing.pop_back();
        }
    }
    if (escaped) {
        json.push_back('\\');
    }
    if (in_string) {
        json.push_back('"');
    }
    while (!json.empty() &&
           (json.back() == ' ' || json.back() == '\t' ||
            json.back() == '\r' || json.back() == '\n')) {
        json.pop_back();
    }
    if (!json.empty() && json.back() == ':') {
        json += "null";
    } else if (!json.empty() && json.back() == ',') {
        json.pop_back();
    }
    for (auto iterator = closing.rbegin(); iterator != closing.rend(); ++iterator) {
        json.push_back(*iterator);
    }
    return json;
}

[[nodiscard]] util::JsonValue parse_streaming_arguments(std::string_view raw_arguments) {
    if (raw_arguments.empty()) {
        return util::JsonValue::object_t{};
    }
    if (auto parsed = util::read_json<util::JsonValue>(raw_arguments)) {
        return std::move(*parsed);
    }
    auto repaired = repair_json_strings(raw_arguments);
    if (auto parsed = util::read_json<util::JsonValue>(repaired)) {
        return std::move(*parsed);
    }
    if (auto parsed = util::read_json<util::JsonValue>(
            complete_partial_json(std::move(repaired)))) {
        return std::move(*parsed);
    }
    return util::JsonValue::object_t{};
}

[[nodiscard]] util::Expected<util::JsonValue> parse_event_json(
    const providers::SseEvent& event) {
    if (auto parsed = util::read_json<util::JsonValue>(event.data)) {
        return std::move(*parsed);
    }
    const auto repaired = repair_json_strings(event.data);
    if (auto parsed = util::read_json<util::JsonValue>(repaired)) {
        return std::move(*parsed);
    }
    return std::unexpected(stream_error(
        "Could not parse Anthropic SSE event " + event.event,
        event.data));
}

[[nodiscard]] AnthropicUsageUpdate usage_update(const JsonObject& usage) {
    AnthropicUsageUpdate update{
        .input = integer_member(usage, "input_tokens"),
        .output = integer_member(usage, "output_tokens"),
        .cache_read = integer_member(usage, "cache_read_input_tokens"),
        .cache_write = integer_member(usage, "cache_creation_input_tokens"),
        .cache_write_1h = std::nullopt,
        .reasoning = std::nullopt,
    };
    if (const auto* cache_creation = object_member(usage, "cache_creation")) {
        update.cache_write_1h = integer_member(
            *cache_creation, "ephemeral_1h_input_tokens");
    }
    if (const auto* output_details = object_member(usage, "output_tokens_details")) {
        update.reasoning = integer_member(*output_details, "thinking_tokens");
    }
    return update;
}

[[nodiscard]] util::ExpectedVoid emit_start(
    AssistantEventSink& sink,
    AssistantMessage& assistant,
    bool& started) {
    if (started) {
        return {};
    }
    started = true;
    return providers::emit(sink, AssistantStartEvent{.partial = assistant});
}

[[nodiscard]] util::ExpectedVoid start_content_block(
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
            .arguments = input ? std::optional<util::JsonValue>{*input}
                               : std::optional<util::JsonValue>{util::JsonValue::object_t{}},
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

[[nodiscard]] util::ExpectedVoid append_content_delta(
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
        block.arguments = parse_streaming_arguments(block.raw_arguments);
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

[[nodiscard]] util::ExpectedVoid stop_content_block(
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
    block.arguments = parse_streaming_arguments(block.raw_arguments);
    block.arguments_valid = true;
    block.argument_error = std::nullopt;
    return providers::emit(sink, ToolCallEndEvent{
        .content_index = slot.content_index,
        .tool_call = block,
        .partial = assistant,
    });
}

[[nodiscard]] util::ExpectedVoid process_json_event(
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

[[nodiscard]] util::ExpectedVoid process_sse_event(
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

struct AnthropicAttemptState {
    providers::SseParser parser;
    std::map<std::size_t, BlockSlot> slots;
    bool started{false};
    bool saw_body{false};
    bool saw_message_start{false};
    bool saw_message_stop{false};
    std::optional<TerminationResult> termination{std::nullopt};
    std::optional<util::Error> handler_failure{std::nullopt};
};

[[nodiscard]] util::ExpectedVoid handle_body_chunk(
    std::string_view bytes,
    const Model& model,
    AssistantMessage& assistant,
    AssistantEventSink& sink,
    AnthropicAttemptState& state) {
    state.saw_body = true;
    if (auto emitted = emit_start(sink, assistant, state.started); !emitted) {
        state.handler_failure = emitted.error();
        return std::unexpected(emitted.error());
    }
    auto events = state.parser.append(bytes);
    if (!events) {
        state.handler_failure = events.error();
        return std::unexpected(events.error());
    }
    for (const auto& event : *events) {
        auto processed = process_sse_event(
            event,
            model,
            assistant,
            state.slots,
            sink,
            state.saw_message_start,
            state.saw_message_stop,
            state.termination);
        if (!processed) {
            state.handler_failure = processed.error();
            return std::unexpected(processed.error());
        }
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid finish_event_parser(
    const Model& model,
    AssistantMessage& assistant,
    AssistantEventSink& sink,
    AnthropicAttemptState& state) {
    auto final_event = state.parser.finish();
    if (!final_event) {
        return std::unexpected(final_event.error());
    }
    if (!*final_event) {
        return {};
    }
    return process_sse_event(
        **final_event,
        model,
        assistant,
        state.slots,
        sink,
        state.saw_message_start,
        state.saw_message_stop,
        state.termination);
}

void finalize_partial_tools(AssistantMessage& assistant) {
    for (auto& content : assistant.content) {
        auto* tool = std::get_if<ToolCallContent>(&content);
        if (!tool) {
            continue;
        }
        tool->arguments = parse_streaming_arguments(tool->raw_arguments);
        tool->arguments_valid = true;
        tool->argument_error = std::nullopt;
    }
}

[[nodiscard]] util::Expected<AssistantMessage> complete_failure(
    AssistantMessage assistant,
    util::Error failure,
    AssistantEventSink& sink) {
    finalize_partial_tools(assistant);
    const auto aborted = failure.code == util::ErrorCode::Cancelled;
    assistant.stop_reason = aborted
        ? AssistantStopReason::Aborted
        : AssistantStopReason::Error;
    if (aborted) {
        assistant.error_message = "Request was aborted";
        failure = util::make_error(util::ErrorCode::Cancelled, *assistant.error_message);
    } else {
        std::string diagnostic = failure.message;
        if (!failure.detail.empty() && diagnostic.find(failure.detail) == std::string::npos) {
            if (!diagnostic.empty()) {
                diagnostic += ": ";
            }
            diagnostic += failure.detail;
        }
        assistant.error_message = providers::bounded_provider_error_detail(
            std::move(diagnostic));
        failure = util::make_error(util::ErrorCode::Stream, *assistant.error_message);
    }
    auto emitted = providers::emit(sink, AssistantErrorEvent{
        .reason = assistant.stop_reason,
        .error = assistant,
        .failure = std::move(failure),
    });
    if (!emitted) {
        return std::unexpected(emitted.error());
    }
    return assistant;
}

[[nodiscard]] providers::ProviderFailure response_failure(
    const providers::StreamResponse& response) {
    ProviderHeaders headers;
    headers.insert(response.head.headers.begin(), response.head.headers.end());
    return providers::ProviderFailure{
        .network_error = false,
        .status = response.head.status_code,
        .headers = std::move(headers),
        .message = response.body,
    };
}

[[nodiscard]] providers::ProviderFailure transport_failure(const util::Error& error) {
    return providers::ProviderFailure{
        .network_error = error.code == util::ErrorCode::Network ||
                         error.code == util::ErrorCode::Timeout,
        .status = std::nullopt,
        .headers = {},
        .message = error.detail.empty() ? error.message : error.detail,
    };
}

[[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> wait_before_retry(
    std::uint64_t delay_ms,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (delay_ms == 0) {
        co_return util::ExpectedVoid{};
    }
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, std::chrono::milliseconds{delay_ms});
    std::stop_callback cancellation{stop_token, [&timer] { timer.cancel(); }};
    boost::system::error_code error;
    co_await timer.async_wait(boost::asio::redirect_error(
        boost::asio::use_awaitable, error));
    if (stop_token.stop_requested()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (error) {
        co_return std::unexpected(stream_error(
            "Anthropic Messages retry wait failed",
            error.message()));
    }
    co_return util::ExpectedVoid{};
}

[[nodiscard]] boost::asio::awaitable<util::Expected<bool>> retry_provider_failure(
    providers::ProviderFailure failure,
    std::uint32_t attempt,
    std::uint32_t max_retries,
    std::uint64_t max_retry_delay_ms,
    std::stop_token stop_token) {
    if (attempt >= max_retries ||
        !providers::is_retryable_provider_failure(failure)) {
        co_return false;
    }
    CCH_TRY(delay, providers::provider_retry_delay_ms(
        failure,
        attempt,
        max_retry_delay_ms,
        current_timestamp_ms()));
    CCH_TRY_VOID(co_await wait_before_retry(delay, stop_token));
    co_return true;
}

} // namespace

AnthropicMessagesAdapter::AnthropicMessagesAdapter(
    std::shared_ptr<providers::StreamTransport> transport)
    : transport_(std::move(transport)) {}

AnthropicMessagesAdapter::AnthropicMessagesAdapter(AnthropicMessagesAdapter&&) noexcept = default;
AnthropicMessagesAdapter& AnthropicMessagesAdapter::operator=(AnthropicMessagesAdapter&&) noexcept = default;
AnthropicMessagesAdapter::~AnthropicMessagesAdapter() = default;

boost::asio::awaitable<util::Expected<AssistantMessage>> AnthropicMessagesAdapter::stream(
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
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
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
    assistant.timestamp = current_timestamp_ms();

    std::optional<util::Error> sink_failure;
    AssistantEventSink guarded_sink =
        [&sink, &sink_failure](const AssistantStreamEvent& event) -> util::ExpectedVoid {
            auto emitted = providers::emit(sink, event);
            if (!emitted) {
                sink_failure = emitted.error();
            }
            return emitted;
        };

    for (std::uint32_t attempt = 0;; ++attempt) {
        AnthropicAttemptState attempt_state;
        auto response = co_await transport_->async_stream(
            request,
            [&](std::string_view bytes) -> util::ExpectedVoid {
                return handle_body_chunk(
                    bytes,
                    model,
                    assistant,
                    guarded_sink,
                    attempt_state);
            });
        if (!response) {
            if (sink_failure) {
                co_return std::unexpected(*sink_failure);
            }
            if (attempt_state.handler_failure) {
                co_return complete_failure(
                    assistant, *attempt_state.handler_failure, guarded_sink);
            }
            if (response.error().code == util::ErrorCode::Cancelled ||
                options.stop_token.stop_requested()) {
                co_return complete_failure(
                    assistant,
                    util::make_error(util::ErrorCode::Cancelled, "Request was aborted"),
                    guarded_sink);
            }
            const auto failure = transport_failure(response.error());
            if (!attempt_state.saw_body) {
                CCH_TRY(retry, co_await retry_provider_failure(
                    failure,
                    attempt,
                    options.max_retries,
                    options.max_retry_delay_ms,
                    options.stop_token));
                if (retry) {
                    continue;
                }
            }
            co_return complete_failure(
                assistant,
                normalize_transport_error(response.error()),
                guarded_sink);
        }

        if (response->head.status_code < 200 || response->head.status_code >= 300) {
            const auto failure = response_failure(*response);
            CCH_TRY(retry, co_await retry_provider_failure(
                failure,
                attempt,
                options.max_retries,
                options.max_retry_delay_ms,
                options.stop_token));
            if (retry) {
                continue;
            }
            co_return complete_failure(
                assistant,
                stream_error(
                    "Anthropic Messages request failed with HTTP " +
                        std::to_string(response->head.status_code),
                    response->body),
                guarded_sink);
        }

        if (!attempt_state.started) {
            if (auto emitted = emit_start(
                    guarded_sink, assistant, attempt_state.started);
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        if (auto finished = finish_event_parser(
                model, assistant, guarded_sink, attempt_state);
            !finished) {
            if (sink_failure) {
                co_return std::unexpected(*sink_failure);
            }
            co_return complete_failure(assistant, finished.error(), guarded_sink);
        }
        if (options.stop_token.stop_requested()) {
            co_return complete_failure(
                assistant,
                util::make_error(util::ErrorCode::Cancelled, "Request was aborted"),
                guarded_sink);
        }
        if (!attempt_state.saw_message_stop) {
            co_return complete_failure(
                assistant,
                stream_error(attempt_state.saw_message_start
                    ? "Anthropic stream ended before message_stop"
                    : "Anthropic stream ended without message_stop"),
                guarded_sink);
        }
        if (!attempt_state.termination) {
            co_return complete_failure(
                assistant,
                stream_error("Anthropic stream ended without a stop reason"),
                guarded_sink);
        }
        if (attempt_state.termination->reason == AssistantStopReason::Error) {
            co_return complete_failure(
                assistant,
                stream_error(attempt_state.termination->error_message.value_or(
                    "Anthropic Messages request failed")),
                guarded_sink);
        }
        assistant.stop_reason = attempt_state.termination->reason;
        assistant.error_message = std::nullopt;
        CCH_TRY_VOID(providers::emit(guarded_sink, AssistantDoneEvent{
            .reason = assistant.stop_reason,
            .message = assistant,
        }));
        co_return assistant;
    }
}

} // namespace cch::ai::api
