#include "OpenAIResponsesAdapter.hpp"

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
using JsonArray = util::JsonValue::array_t;

struct OutputSlot {
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

[[nodiscard]] const JsonArray* array_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    return found ? found->get_if<JsonArray>() : nullptr;
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

[[nodiscard]] std::optional<std::size_t> output_index(const JsonObject& event) {
    const auto value = integer_member(event, "output_index");
    if (!value) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*value);
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
void set_header(
    Headers& headers,
    std::string name,
    std::string value) {
    std::erase_if(headers, [&name](const auto& header) {
        return header_name_equal(header.first, name);
    });
    headers.emplace(std::move(name), std::move(value));
}

template <typename Headers>
[[nodiscard]] bool has_header(
    const Headers& headers,
    std::string_view name) {
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

[[nodiscard]] std::string responses_url(std::string_view base_url) {
    std::string result{base_url};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result + "/responses";
}

[[nodiscard]] util::Expected<providers::StreamRequest> build_stream_request(
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
    auto payload = build_adapter_payload(
        AdapterKind::OpenAIResponses, model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = util::write_json(*payload);
    if (!body) {
        return std::unexpected(body.error());
    }

    providers::StreamRequest request;
    request.url = responses_url(model.base_url);
    request.timeout = std::chrono::milliseconds{
        options.timeout_ms.value_or(30000)};
    request.stop_token = options.stop_token;
    request.headers.insert(
        options.auth.headers.begin(), options.auth.headers.end());
    if (options.auth.api_key &&
        !has_header(request.headers, "authorization") &&
        !header_deleted(options, "authorization")) {
        set_header(
            request.headers,
            "Authorization",
            "Bearer " + *options.auth.api_key);
    }
    if (!has_header(request.headers, "content-type") &&
        !header_deleted(options, "content-type")) {
        set_header(request.headers, "Content-Type", "application/json");
    }
    if (!has_header(request.headers, "accept") &&
        !header_deleted(options, "accept")) {
        set_header(request.headers, "Accept", "text/event-stream");
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
        error.message.empty() ? "OpenAI Responses request failed" : error.message,
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
                std::ranges::all_of(
                    json.substr(index + 2, 4),
                    [](char digit) {
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

[[nodiscard]] util::ExpectedVoid finalize_tool_arguments(ToolCallContent& tool) {
    tool.arguments = parse_streaming_arguments(tool.raw_arguments);
    tool.arguments_valid = true;
    tool.argument_error = std::nullopt;
    return {};
}

[[nodiscard]] std::string joined_item_text(
    const JsonObject& item,
    std::string_view array_name) {
    const auto* entries = array_member(item, array_name);
    if (!entries) {
        return {};
    }
    std::string result;
    for (const auto& entry : *entries) {
        const auto* entry_object = object(entry);
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
        const auto* entry_object = object(entry);
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

[[nodiscard]] util::Expected<std::string> text_signature(const JsonObject& item) {
    util::JsonValue::object_t signature{
        {"id", std::string{string_member(item, "id").value_or("")}},
        {"v", 1},
    };
    if (const auto phase = string_member(item, "phase");
        phase == "commentary" || phase == "final_answer") {
        signature.emplace("phase", std::string{*phase});
    }
    return util::write_json(util::JsonValue{std::move(signature)});
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

[[nodiscard]] util::ExpectedVoid create_slot(
    std::size_t index,
    const JsonObject& item,
    std::map<std::size_t, OutputSlot>& slots,
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
        slots.emplace(index, OutputSlot{
            .kind = OutputSlot::Kind::Thinking,
            .content_index = content_index,
            .partial_arguments = {},
        });
        return providers::emit(sink, ThinkingStartEvent{
            .content_index = content_index,
            .partial = assistant,
        });
    }
    if (*type == "message") {
        const auto content_index = assistant.content.size();
        assistant.content.emplace_back(TextContent{});
        slots.emplace(index, OutputSlot{
            .kind = OutputSlot::Kind::Text,
            .content_index = content_index,
            .partial_arguments = {},
        });
        return providers::emit(sink, TextStartEvent{
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
            .arguments = std::nullopt,
            .raw_arguments = arguments,
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
        });
        slots.emplace(index, OutputSlot{
            .kind = OutputSlot::Kind::ToolCall,
            .content_index = content_index,
            .partial_arguments = std::move(arguments),
        });
        return providers::emit(sink, ToolCallStartEvent{
            .content_index = content_index,
            .partial = assistant,
        });
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid append_delta(
    const JsonObject& event,
    std::string_view type,
    std::map<std::size_t, OutputSlot>& slots,
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
    if ((type == "response.reasoning_summary_text.delta" ||
         type == "response.reasoning_text.delta") &&
        slot.kind == OutputSlot::Kind::Thinking) {
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        block.thinking += *delta;
        return providers::emit(sink, ThinkingDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{*delta},
            .partial = assistant,
        });
    }
    if ((type == "response.output_text.delta" || type == "response.refusal.delta") &&
        slot.kind == OutputSlot::Kind::Text) {
        auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        block.text += *delta;
        return providers::emit(sink, TextDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{*delta},
            .partial = assistant,
        });
    }
    if (type == "response.function_call_arguments.delta" &&
        slot.kind == OutputSlot::Kind::ToolCall) {
        slot.partial_arguments += *delta;
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        block.raw_arguments = slot.partial_arguments;
        block.arguments = parse_streaming_arguments(block.raw_arguments);
        block.arguments_valid = true;
        block.argument_error = std::nullopt;
        return providers::emit(sink, ToolCallDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{*delta},
            .partial = assistant,
        });
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid append_reasoning_separator(
    const JsonObject& event,
    std::map<std::size_t, OutputSlot>& slots,
    AssistantMessage& assistant,
    AssistantEventSink& sink) {
    const auto index = output_index(event);
    if (!index) {
        return {};
    }
    const auto found = slots.find(*index);
    if (found == slots.end() || found->second.kind != OutputSlot::Kind::Thinking) {
        return {};
    }
    auto& block = std::get<ThinkingContent>(
        assistant.content[found->second.content_index]);
    block.thinking += "\n\n";
    return providers::emit(sink, ThinkingDeltaEvent{
        .content_index = found->second.content_index,
        .delta = "\n\n",
        .partial = assistant,
    });
}

[[nodiscard]] util::ExpectedVoid finish_argument_stream(
    const JsonObject& event,
    std::map<std::size_t, OutputSlot>& slots,
    AssistantMessage& assistant,
    AssistantEventSink& sink) {
    const auto index = output_index(event);
    const auto arguments = string_member(event, "arguments");
    if (!index || !arguments) {
        return {};
    }
    const auto found = slots.find(*index);
    if (found == slots.end() || found->second.kind != OutputSlot::Kind::ToolCall) {
        return {};
    }
    auto& slot = found->second;
    const auto previous = slot.partial_arguments;
    slot.partial_arguments = *arguments;
    auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
    block.raw_arguments = slot.partial_arguments;
    block.arguments = parse_streaming_arguments(block.raw_arguments);
    block.arguments_valid = true;
    block.argument_error = std::nullopt;
    if (arguments->starts_with(previous) && arguments->size() > previous.size()) {
        return providers::emit(sink, ToolCallDeltaEvent{
            .content_index = slot.content_index,
            .delta = std::string{arguments->substr(previous.size())},
            .partial = assistant,
        });
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid finish_output_item(
    const JsonObject& event,
    std::map<std::size_t, OutputSlot>& slots,
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
    if (type == "reasoning" && slot.kind == OutputSlot::Kind::Thinking) {
        auto& block = std::get<ThinkingContent>(assistant.content[slot.content_index]);
        auto content = joined_item_text(*item, "summary");
        if (content.empty()) {
            content = joined_item_text(*item, "content");
        }
        if (!content.empty()) {
            block.thinking = std::move(content);
        }
        auto signature = util::write_json(util::JsonValue{*item});
        if (!signature) {
            return std::unexpected(signature.error());
        }
        block.thinking_signature = std::move(*signature);
        if (auto emitted = providers::emit(sink, ThinkingEndEvent{
                .content_index = slot.content_index,
                .content = block.thinking,
                .partial = assistant,
            }); !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
        return {};
    }
    if (type == "message" && slot.kind == OutputSlot::Kind::Text) {
        auto& block = std::get<TextContent>(assistant.content[slot.content_index]);
        block.text = joined_message_text(*item);
        auto signature = text_signature(*item);
        if (!signature) {
            return std::unexpected(signature.error());
        }
        block.text_signature = std::move(*signature);
        if (auto emitted = providers::emit(sink, TextEndEvent{
                .content_index = slot.content_index,
                .content = block.text,
                .partial = assistant,
            }); !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
        return {};
    }
    if (type == "function_call" && slot.kind == OutputSlot::Kind::ToolCall) {
        auto& block = std::get<ToolCallContent>(assistant.content[slot.content_index]);
        block.id = std::string{string_member(*item, "call_id").value_or("")} + "|" +
                   std::string{string_member(*item, "id").value_or("")};
        block.name = std::string{string_member(*item, "name").value_or("")};
        block.raw_arguments = std::string{
            string_member(*item, "arguments").value_or(slot.partial_arguments)};
        if (auto finalized = finalize_tool_arguments(block); !finalized) {
            return std::unexpected(finalized.error());
        }
        if (auto emitted = providers::emit(sink, ToolCallEndEvent{
                .content_index = slot.content_index,
                .tool_call = block,
                .partial = assistant,
            }); !emitted) {
            return std::unexpected(emitted.error());
        }
        slots.erase(found);
    }
    return {};
}

void apply_usage(
    const Model& model,
    const JsonObject& response,
    AssistantMessage& assistant) {
    const auto* usage = object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = object_member(*usage, "input_tokens_details");
    const auto* output_details = object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_deepseek_usage(
        model,
        integer_member(*usage, "input_tokens").value_or(0),
        integer_member(*usage, "output_tokens").value_or(0),
        input_details ? integer_member(*input_details, "cached_tokens").value_or(0) : 0,
        output_details
            ? integer_member(*output_details, "reasoning_tokens")
            : std::nullopt);
    if (const auto total = integer_member(*usage, "total_tokens")) {
        assistant.usage.total_tokens = *total;
    }
}

[[nodiscard]] util::ExpectedVoid finalize_response(
    const Model& model,
    const JsonObject& event,
    std::string_view event_type,
    AssistantMessage& assistant,
    bool& saw_terminal) {
    const auto* response = object_member(event, "response");
    if (!response) {
        return std::unexpected(stream_error(
            "OpenAI Responses terminal event omitted response data"));
    }
    saw_terminal = true;
    if (const auto id = string_member(*response, "id"); id && !id->empty()) {
        assistant.response_id = std::string{*id};
    }
    if (const auto model_id = string_member(*response, "model");
        model_id && *model_id != model.id) {
        assistant.response_model = std::string{*model_id};
    }
    apply_usage(model, *response, assistant);
    if (event_type == "response.failed") {
        const auto* error = object_member(*response, "error");
        const auto code = error ? string_member(*error, "code") : std::nullopt;
        const auto message = error ? string_member(*error, "message") : std::nullopt;
        std::string detail = code ? std::string{*code} : "unknown";
        detail += ": ";
        detail += message ? std::string{*message} : "no message";
        return std::unexpected(stream_error(detail));
    }
    const auto status = string_member(*response, "status").value_or(
        event_type == "response.done" ? "done" : "");
    auto termination = map_responses_termination(
        status,
        std::ranges::any_of(assistant.content, [](const AssistantContent& block) {
            return std::holds_alternative<ToolCallContent>(block);
        }));
    if (!termination) {
        return std::unexpected(termination.error());
    }
    assistant.stop_reason = termination->reason;
    assistant.error_message = termination->error_message;
    return {};
}

[[nodiscard]] util::ExpectedVoid process_json_event(
    const Model& model,
    const JsonObject& event,
    AssistantMessage& assistant,
    std::map<std::size_t, OutputSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_terminal) {
    const auto type = string_member(event, "type");
    if (!type) {
        return {};
    }
    if (*type == "response.created") {
        if (const auto* response = object_member(event, "response")) {
            if (const auto id = string_member(*response, "id"); id && !id->empty()) {
                assistant.response_id = std::string{*id};
            }
        }
        return {};
    }
    if (*type == "response.output_item.added") {
        const auto index = output_index(event);
        const auto* item = object_member(event, "item");
        return index && item ? create_slot(*index, *item, slots, assistant, sink)
                             : util::ExpectedVoid{};
    }
    if (*type == "response.reasoning_summary_text.delta" ||
        *type == "response.reasoning_text.delta" ||
        *type == "response.output_text.delta" ||
        *type == "response.refusal.delta" ||
        *type == "response.function_call_arguments.delta") {
        return append_delta(event, *type, slots, assistant, sink);
    }
    if (*type == "response.reasoning_summary_part.done") {
        return append_reasoning_separator(event, slots, assistant, sink);
    }
    if (*type == "response.function_call_arguments.done") {
        return finish_argument_stream(event, slots, assistant, sink);
    }
    if (*type == "response.output_item.done") {
        return finish_output_item(event, slots, assistant, sink);
    }
    if (*type == "response.completed" || *type == "response.done" ||
        *type == "response.incomplete" || *type == "response.failed") {
        return finalize_response(model, event, *type, assistant, saw_terminal);
    }
    if (*type == "error") {
        const auto code = string_member(event, "code").value_or("unknown");
        const auto message = string_member(event, "message").value_or("Unknown error");
        return std::unexpected(stream_error(
            "Error Code " + std::string{code} + ": " + std::string{message}));
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid process_sse_event(
    const providers::SseEvent& event,
    const Model& model,
    AssistantMessage& assistant,
    std::map<std::size_t, OutputSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_terminal) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream_error(event.data));
    }
    auto parsed = util::read_json<util::JsonValue>(event.data);
    if (!parsed) {
        if (event.event != "message" && !event.event.starts_with("response.")) {
            return {};
        }
        return std::unexpected(stream_error(
            "Malformed OpenAI Responses SSE event",
            parsed.error().detail));
    }
    const auto* event_object = object(*parsed);
    if (!event_object) {
        return std::unexpected(stream_error(
            "Malformed OpenAI Responses SSE event",
            "event data must be a JSON object"));
    }
    return process_json_event(
        model, *event_object, assistant, slots, sink, saw_terminal);
}

[[nodiscard]] util::Expected<AssistantMessage> complete_failure(
    AssistantMessage assistant,
    util::Error failure,
    AssistantEventSink& sink) {
    for (auto& block : assistant.content) {
        auto* tool = std::get_if<ToolCallContent>(&block);
        if (tool && !tool->arguments) {
            if (auto finalized = finalize_tool_arguments(*tool); !finalized) {
                return std::unexpected(finalized.error());
            }
        }
    }
    const auto aborted = failure.code == util::ErrorCode::Cancelled;
    assistant.stop_reason = aborted
        ? AssistantStopReason::Aborted
        : AssistantStopReason::Error;
    if (aborted) {
        assistant.error_message = "Request was aborted";
        failure = util::make_error(
            util::ErrorCode::Cancelled,
            *assistant.error_message);
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
        failure = util::make_error(
            util::ErrorCode::Stream,
            *assistant.error_message);
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
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Stream,
            "OpenAI Responses retry wait failed",
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

OpenAIResponsesAdapter::OpenAIResponsesAdapter(
    std::shared_ptr<providers::StreamTransport> transport)
    : transport_(std::move(transport)) {}

OpenAIResponsesAdapter::OpenAIResponsesAdapter(OpenAIResponsesAdapter&&) noexcept = default;
OpenAIResponsesAdapter& OpenAIResponsesAdapter::operator=(OpenAIResponsesAdapter&&) noexcept = default;
OpenAIResponsesAdapter::~OpenAIResponsesAdapter() = default;

boost::asio::awaitable<util::Expected<AssistantMessage>> OpenAIResponsesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!transport_) {
        co_return std::unexpected(stream_error(
            "OpenAI Responses adapter requires a stream transport"));
    }
    if (model.api != "openai-responses") {
        co_return std::unexpected(stream_error(
            "OpenAI Responses adapter received the wrong Model API"));
    }
    if (model.base_url.empty()) {
        co_return std::unexpected(stream_error(
            "OpenAI Responses Model base URL is required"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if ((!options.auth.api_key || options.auth.api_key->empty()) &&
        !has_header(options.auth.headers, "authorization") &&
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
        providers::SseParser parser;
        std::map<std::size_t, OutputSlot> slots;
        bool saw_terminal = false;
        bool started = false;
        bool saw_body = false;
        std::optional<util::Error> handler_failure;

        auto handle_chunk = [&](std::string_view bytes) -> util::ExpectedVoid {
            saw_body = true;
            if (auto emitted = emit_start(guarded_sink, assistant, started); !emitted) {
                handler_failure = emitted.error();
                return std::unexpected(emitted.error());
            }
            auto events = parser.append(bytes);
            if (!events) {
                handler_failure = events.error();
                return std::unexpected(events.error());
            }
            for (const auto& event : *events) {
                auto processed = process_sse_event(
                    event, model, assistant, slots, guarded_sink, saw_terminal);
                if (!processed) {
                    handler_failure = processed.error();
                    return std::unexpected(processed.error());
                }
            }
            return {};
        };

        auto response = co_await transport_->async_stream(request, handle_chunk);
        if (!response) {
            if (sink_failure) {
                co_return std::unexpected(*sink_failure);
            }
            if (handler_failure) {
                co_return complete_failure(
                    assistant, *handler_failure, guarded_sink);
            }
            if (response.error().code == util::ErrorCode::Cancelled ||
                options.stop_token.stop_requested()) {
                co_return complete_failure(
                    assistant,
                    util::make_error(
                        util::ErrorCode::Cancelled,
                        "Request was aborted"),
                    guarded_sink);
            }
            const auto failure = transport_failure(response.error());
            if (!saw_body) {
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
                    "OpenAI Responses request failed with HTTP " +
                        std::to_string(response->head.status_code),
                    response->body),
                guarded_sink);
        }

        if (!started) {
            if (auto emitted = emit_start(guarded_sink, assistant, started); !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        auto final_event = parser.finish();
        if (!final_event) {
            co_return complete_failure(
                assistant, final_event.error(), guarded_sink);
        }
        if (*final_event) {
            if (auto processed = process_sse_event(
                    **final_event,
                    model,
                    assistant,
                    slots,
                    guarded_sink,
                    saw_terminal);
                !processed) {
                if (sink_failure) {
                    co_return std::unexpected(*sink_failure);
                }
                co_return complete_failure(
                    assistant, processed.error(), guarded_sink);
            }
        }
        if (options.stop_token.stop_requested()) {
            co_return complete_failure(
                assistant,
                util::make_error(
                    util::ErrorCode::Cancelled,
                    "Request was aborted"),
                guarded_sink);
        }
        if (!saw_terminal) {
            co_return complete_failure(
                assistant,
                stream_error(
                    "OpenAI Responses stream ended before a terminal response event"),
                guarded_sink);
        }
        if (assistant.stop_reason == AssistantStopReason::Error) {
            co_return complete_failure(
                assistant,
                stream_error(assistant.error_message.value_or(
                    "OpenAI Responses request failed")),
                guarded_sink);
        }
        CCH_TRY_VOID(providers::emit(guarded_sink, AssistantDoneEvent{
            .reason = assistant.stop_reason,
            .message = assistant,
        }));
        co_return assistant;
    }
}

} // namespace cch::ai::api
