#pragma once

// Shared OpenAI Responses stream event processing used by the private
// `openai-responses` (DeepSeek) and `openai-codex-responses` adapters
// (ADR 0033). Both adapters consume the same Responses wire events; only the
// terminal/error policy differs, which stays in each adapter.

#include <cch/ai/Provider.hpp>
#include "PartialJson.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace responses_stream {

using JsonObject = util::JsonValue::object_t;
using JsonArray = util::JsonValue::array_t;

struct OutputSlot {
    enum class Kind { Thinking, Text, ToolCall };

    Kind kind{Kind::Text};
    std::size_t content_index{};
    std::string partial_arguments;
};

[[nodiscard]] inline TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] inline const JsonObject* object(const util::JsonValue& value) {
    return value.get_if<JsonObject>();
}

[[nodiscard]] inline const util::JsonValue* member(
    const JsonObject& value,
    std::string_view name) {
    const auto found = value.find(std::string{name});
    return found == value.end() ? nullptr : &found->second;
}

[[nodiscard]] inline const JsonObject* object_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    return found ? found->get_if<JsonObject>() : nullptr;
}

[[nodiscard]] inline const JsonArray* array_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    return found ? found->get_if<JsonArray>() : nullptr;
}

[[nodiscard]] inline std::optional<std::string_view> string_member(
    const JsonObject& value,
    std::string_view name) {
    const auto* found = member(value, name);
    const auto* text = found ? found->get_if<std::string>() : nullptr;
    return text ? std::optional<std::string_view>{*text} : std::nullopt;
}

[[nodiscard]] inline std::optional<std::int64_t> integer_member(
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

[[nodiscard]] inline std::optional<std::size_t> output_index(const JsonObject& event) {
    const auto value = integer_member(event, "output_index");
    if (!value) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*value);
}

[[nodiscard]] inline bool header_name_equal(
    std::string_view left,
    std::string_view right) {
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
inline void set_header(
    Headers& headers,
    std::string name,
    std::string value) {
    std::erase_if(headers, [&name](const auto& header) {
        return header_name_equal(header.first, name);
    });
    headers.emplace(std::move(name), std::move(value));
}

template <typename Headers>
[[nodiscard]] inline bool has_header(
    const Headers& headers,
    std::string_view name) {
    return std::ranges::any_of(headers, [name](const auto& header) {
        return header_name_equal(header.first, name) && !header.second.empty();
    });
}

[[nodiscard]] inline bool header_deleted(
    const ProviderStreamOptions& options,
    std::string_view name) {
    return std::ranges::any_of(options.deleted_headers, [name](const auto& header) {
        return header_name_equal(header, name);
    });
}

[[nodiscard]] inline util::Error stream_error(
    std::string message,
    std::string detail = {}) {
    return util::make_error(
        util::ErrorCode::Stream,
        providers::bounded_provider_error_detail(std::move(message)),
        providers::bounded_provider_error_detail(std::move(detail)));
}

[[nodiscard]] inline util::JsonValue parse_streaming_arguments(
    std::string_view raw_arguments) {
    // Streaming-tolerant argument parsing matching pi's `parseStreamingJson`
    // (partial-json semantics), shared with the Anthropic adapter.
    return parse_streaming_json(raw_arguments);
}

[[nodiscard]] inline util::ExpectedVoid finalize_tool_arguments(ToolCallContent& tool) {
    tool.arguments = parse_streaming_arguments(tool.raw_arguments);
    tool.arguments_valid = true;
    tool.argument_error = std::nullopt;
    return {};
}

[[nodiscard]] inline std::string joined_item_text(
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

[[nodiscard]] inline std::string joined_message_text(const JsonObject& item) {
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

/// Text signature v1: {"id": <item-id>, "v": 1, "phase": "commentary"|"final_answer"}
[[nodiscard]] inline util::Expected<std::string> text_signature(const JsonObject& item) {
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

inline void apply_message_phase_stop_reason(
    AssistantMessage& assistant,
    const JsonObject& item) {
    // pi's `applyMessagePhaseStopReason` (openai-responses-shared.ts): a
    // message output item whose `phase` is `final_answer` flips the running
    // partial from `pending` to `stop` before the item is surfaced.
    if (const auto type = string_member(item, "type");
        type && *type == "message") {
        if (const auto phase = string_member(item, "phase");
            phase && *phase == "final_answer") {
            assistant.stop_reason = AssistantStopReason::Stop;
        }
    }
}

[[nodiscard]] inline util::ExpectedVoid emit_start(
    AssistantEventSink& sink,
    AssistantMessage& assistant,
    bool& started) {
    if (started) {
        return {};
    }
    started = true;
    return providers::emit(sink, AssistantStartEvent{.partial = assistant});
}

[[nodiscard]] inline util::ExpectedVoid create_slot(
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
        apply_message_phase_stop_reason(assistant, item);
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
            // pi constructs every tool call with an empty arguments object
            // (parseStreamingJson("")) and fills it from the raw arguments.
            .arguments = util::JsonValue{util::JsonValue::object_t{}},
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

[[nodiscard]] inline util::ExpectedVoid append_delta(
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

[[nodiscard]] inline util::ExpectedVoid append_reasoning_separator(
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

[[nodiscard]] inline util::ExpectedVoid finish_argument_stream(
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

[[nodiscard]] inline util::ExpectedVoid finish_output_item(
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
        apply_message_phase_stop_reason(assistant, *item);
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

/// Emits one terminal error event carrying the final assistant value and turns
/// the failure into the stream/cancelled category.
[[nodiscard]] inline util::Expected<AssistantMessage> complete_failure(
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

[[nodiscard]] inline providers::ProviderFailure response_failure(
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

[[nodiscard]] inline providers::ProviderFailure transport_failure(
    const util::Error& error) {
    return providers::ProviderFailure{
        .network_error = error.code == util::ErrorCode::Network ||
                         error.code == util::ErrorCode::Timeout,
        .status = std::nullopt,
        .headers = {},
        .message = error.detail.empty() ? error.message : error.detail,
    };
}

[[nodiscard]] inline boost::asio::awaitable<util::ExpectedVoid> wait_before_retry(
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
            "Responses retry wait failed",
            error.message()));
    }
    co_return util::ExpectedVoid{};
}

[[nodiscard]] inline boost::asio::awaitable<util::Expected<bool>> retry_provider_failure(
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

} // namespace responses_stream
} // namespace cch::ai::api
