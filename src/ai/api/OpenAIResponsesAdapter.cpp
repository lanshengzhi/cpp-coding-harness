#include "OpenAIResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ResponsesStream.hpp"
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

namespace stream = responses_stream;

using JsonObject = support::JsonValue::object_t;
using JsonArray = support::JsonValue::array_t;

[[nodiscard]] std::string responses_url(std::string_view base_url) {
    std::string result{base_url};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result + "/responses";
}

[[nodiscard]] support::Expected<providers::StreamRequest> build_stream_request(
    const Model& model,
    const AiContext& context,
    const ProviderStreamOptions& options) {
    auto payload = build_adapter_payload(
        AdapterKind::OpenAIResponses, model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = support::write_json(*payload);
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
        !stream::has_header(request.headers, "authorization") &&
        !stream::header_deleted(options, "authorization")) {
        stream::set_header(
            request.headers,
            "Authorization",
            "Bearer " + *options.auth.api_key);
    }
    if (!stream::has_header(request.headers, "content-type") &&
        !stream::header_deleted(options, "content-type")) {
        stream::set_header(request.headers, "Content-Type", "application/json");
    }
    if (!stream::has_header(request.headers, "accept") &&
        !stream::header_deleted(options, "accept")) {
        stream::set_header(request.headers, "Accept", "text/event-stream");
    }
    request.body = std::move(*body);
    return request;
}


void apply_usage(
    const Model& model,
    const JsonObject& response,
    AssistantMessage& assistant) {
    const auto* usage = stream::object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = stream::object_member(*usage, "input_tokens_details");
    const auto* output_details = stream::object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_deepseek_usage(
        model,
        stream::integer_member(*usage, "input_tokens").value_or(0),
        stream::integer_member(*usage, "output_tokens").value_or(0),
        input_details ? stream::integer_member(*input_details, "cached_tokens").value_or(0) : 0,
        output_details
            ? stream::integer_member(*output_details, "reasoning_tokens")
            : std::nullopt);
    if (const auto total = stream::integer_member(*usage, "total_tokens")) {
        assistant.usage.total_tokens = *total;
    }
}

[[nodiscard]] support::ExpectedVoid finalize_response(
    const Model& model,
    const JsonObject& event,
    std::string_view event_type,
    AssistantMessage& assistant,
    bool& saw_terminal) {
    // pi's `processResponsesStream` records that a terminal response event
    // arrived (`sawTerminalResponseEvent`); the shared guard errors with
    // "OpenAI Responses stream ended before a terminal response event" when
    // none did, before the wrapper's defensive pending check ever runs.
    saw_terminal = true;
    const auto* response = stream::object_member(event, "response");
    if (!response) {
        return std::unexpected(stream::stream_error(
            "OpenAI Responses terminal event omitted response data"));
    }
    if (const auto id = stream::string_member(*response, "id"); id && !id->empty()) {
        assistant.response_id = std::string{*id};
    }
    if (const auto model_id = stream::string_member(*response, "model");
        model_id && *model_id != model.id) {
        assistant.response_model = std::string{*model_id};
    }
    // pi's `finalizeResponse` records the raw wire status as `rawStopReason`
    // for every terminal Responses event, including failures
    // (openai-responses-shared.ts).
    if (const auto status = stream::string_member(*response, "status");
        status && !status->empty()) {
        assistant.raw_stop_reason = *status;
    }
    apply_usage(model, *response, assistant);
    if (event_type == "response.failed") {
        const auto* error = stream::object_member(*response, "error");
        const auto code = error ? stream::string_member(*error, "code") : std::nullopt;
        const auto message = error ? stream::string_member(*error, "message") : std::nullopt;
        std::string detail = code ? std::string{*code} : "unknown";
        detail += ": ";
        detail += message ? std::string{*message} : "no message";
        return std::unexpected(stream::stream_error(detail));
    }
    const auto status = stream::string_member(*response, "status").value_or(
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

[[nodiscard]] support::ExpectedVoid process_json_event(
    const Model& model,
    const JsonObject& event,
    AssistantMessage& assistant,
    std::map<std::size_t, stream::OutputSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_terminal) {
    const auto type = stream::string_member(event, "type");
    if (!type) {
        return {};
    }
    if (*type == "response.created") {
        if (const auto* response = stream::object_member(event, "response")) {
            if (const auto id = stream::string_member(*response, "id"); id && !id->empty()) {
                assistant.response_id = std::string{*id};
            }
        }
        return {};
    }
    if (*type == "response.output_item.added") {
        const auto index = stream::output_index(event);
        const auto* item = stream::object_member(event, "item");
        return index && item
            ? stream::create_slot(*index, *item, slots, assistant, sink)
            : support::ExpectedVoid{};
    }
    if (*type == "response.reasoning_summary_text.delta" ||
        *type == "response.reasoning_text.delta" ||
        *type == "response.output_text.delta" ||
        *type == "response.refusal.delta" ||
        *type == "response.function_call_arguments.delta") {
        return stream::append_delta(event, *type, slots, assistant, sink);
    }
    if (*type == "response.reasoning_summary_part.done") {
        return stream::append_reasoning_separator(event, slots, assistant, sink);
    }
    if (*type == "response.function_call_arguments.done") {
        return stream::finish_argument_stream(event, slots, assistant, sink);
    }
    if (*type == "response.output_item.done") {
        return stream::finish_output_item(event, slots, assistant, sink);
    }
    if (*type == "response.completed" || *type == "response.done" ||
        *type == "response.incomplete" || *type == "response.failed") {
        return finalize_response(model, event, *type, assistant, saw_terminal);
    }
    if (*type == "error") {
        const auto code = stream::string_member(event, "code").value_or("unknown");
        const auto message = stream::string_member(event, "message").value_or("Unknown error");
        return std::unexpected(stream::stream_error(
            "Error Code " + std::string{code} + ": " + std::string{message}));
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid process_sse_event(
    const providers::SseEvent& event,
    const Model& model,
    AssistantMessage& assistant,
    std::map<std::size_t, stream::OutputSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_terminal) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream::stream_error(event.data));
    }
    auto parsed = support::read_json(event.data);
    if (!parsed) {
        if (event.event != "message" && !event.event.starts_with("response.")) {
            return {};
        }
        return std::unexpected(stream::stream_error(
            "Malformed OpenAI Responses SSE event",
            parsed.error().detail));
    }
    const auto* event_object = stream::object(*parsed);
    if (!event_object) {
        return std::unexpected(stream::stream_error(
            "Malformed OpenAI Responses SSE event",
            "event data must be a JSON object"));
    }
    return process_json_event(
        model, *event_object, assistant, slots, sink, saw_terminal);
}

} // namespace

OpenAIResponsesAdapter::OpenAIResponsesAdapter(
    std::shared_ptr<providers::StreamTransport> transport)
    : transport_(std::move(transport)) {}

OpenAIResponsesAdapter::OpenAIResponsesAdapter(OpenAIResponsesAdapter&&) noexcept = default;
OpenAIResponsesAdapter& OpenAIResponsesAdapter::operator=(OpenAIResponsesAdapter&&) noexcept = default;
OpenAIResponsesAdapter::~OpenAIResponsesAdapter() = default;

boost::asio::awaitable<support::Expected<AssistantMessage>> OpenAIResponsesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!transport_) {
        co_return std::unexpected(stream::stream_error(
            "OpenAI Responses adapter requires a stream transport"));
    }
    if (model.api != "openai-responses") {
        co_return std::unexpected(stream::stream_error(
            "OpenAI Responses adapter received the wrong Model API"));
    }
    if (model.base_url.empty()) {
        co_return std::unexpected(stream::stream_error(
            "OpenAI Responses Model base URL is required"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if ((!options.auth.api_key || options.auth.api_key->empty()) &&
        !stream::has_header(options.auth.headers, "authorization") &&
        !stream::has_header(options.auth.headers, "cf-aig-authorization")) {
        co_return std::unexpected(stream::stream_error(
            "No API key for provider: " + model.provider));
    }

    CCH_TRY(request, build_stream_request(model, context, options));

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = stream::current_timestamp_ms();

    struct AttemptState {
        std::map<std::size_t, stream::OutputSlot> slots{};
        bool saw_terminal{false};
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
                attempt_state->saw_terminal);
        };
    };

    auto finalize_hook = [attempt_state](AssistantMessage& assistant) -> support::ExpectedVoid {
        if (!attempt_state->saw_terminal) {
            return std::unexpected(stream::stream_error(
                "OpenAI Responses stream ended before a terminal response event"));
        }
        if (assistant.stop_reason == AssistantStopReason::Error) {
            return std::unexpected(stream::stream_error(
                assistant.error_message.value_or("OpenAI Responses request failed")));
        }
        return {};
    };

    co_return co_await providers::execute_sse_stream(providers::SseStreamExecutionOptions{
        .protocol_name = "OpenAI Responses",
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
