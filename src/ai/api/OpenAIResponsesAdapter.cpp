#include "OpenAIResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ResponsesStream.hpp"
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

namespace stream = responses_stream;

using JsonObject = util::JsonValue::object_t;
using JsonArray = util::JsonValue::array_t;

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

[[nodiscard]] util::Error normalize_transport_error(const util::Error& error) {
    if (error.code == util::ErrorCode::Cancelled) {
        return util::make_error(util::ErrorCode::Cancelled, "Request was aborted");
    }
    if (error.code == util::ErrorCode::Unknown) {
        return error;
    }
    return stream::stream_error(
        error.message.empty() ? "OpenAI Responses request failed" : error.message,
        error.detail);
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

[[nodiscard]] util::ExpectedVoid finalize_response(
    const Model& model,
    const JsonObject& event,
    std::string_view event_type,
    AssistantMessage& assistant) {
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

[[nodiscard]] util::ExpectedVoid process_json_event(
    const Model& model,
    const JsonObject& event,
    AssistantMessage& assistant,
    std::map<std::size_t, stream::OutputSlot>& slots,
    AssistantEventSink& sink) {
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
            : util::ExpectedVoid{};
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
        return finalize_response(model, event, *type, assistant);
    }
    if (*type == "error") {
        const auto code = stream::string_member(event, "code").value_or("unknown");
        const auto message = stream::string_member(event, "message").value_or("Unknown error");
        return std::unexpected(stream::stream_error(
            "Error Code " + std::string{code} + ": " + std::string{message}));
    }
    return {};
}

[[nodiscard]] util::ExpectedVoid process_sse_event(
    const providers::SseEvent& event,
    const Model& model,
    AssistantMessage& assistant,
    std::map<std::size_t, stream::OutputSlot>& slots,
    AssistantEventSink& sink) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream::stream_error(event.data));
    }
    auto parsed = util::read_json<util::JsonValue>(event.data);
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
        model, *event_object, assistant, slots, sink);
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
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
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
        std::map<std::size_t, stream::OutputSlot> slots;
        bool started = false;
        bool saw_body = false;
        std::optional<util::Error> handler_failure;

        auto handle_chunk = [&](std::string_view bytes) -> util::ExpectedVoid {
            saw_body = true;
            if (auto emitted = stream::emit_start(guarded_sink, assistant, started); !emitted) {
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
                    event, model, assistant, slots, guarded_sink);
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
                co_return stream::complete_failure(
                    assistant, *handler_failure, guarded_sink);
            }
            if (response.error().code == util::ErrorCode::Cancelled ||
                options.stop_token.stop_requested()) {
                co_return stream::complete_failure(
                    assistant,
                    util::make_error(
                        util::ErrorCode::Cancelled,
                        "Request was aborted"),
                    guarded_sink);
            }
            const auto failure = stream::transport_failure(response.error());
            if (!saw_body) {
                CCH_TRY(retry, co_await stream::retry_provider_failure(
                    failure,
                    attempt,
                    options.max_retries,
                    options.max_retry_delay_ms,
                    options.stop_token));
                if (retry) {
                    continue;
                }
            }
            co_return stream::complete_failure(
                assistant,
                normalize_transport_error(response.error()),
                guarded_sink);
        }

        if (response->head.status_code < 200 || response->head.status_code >= 300) {
            const auto failure = stream::response_failure(*response);
            CCH_TRY(retry, co_await stream::retry_provider_failure(
                failure,
                attempt,
                options.max_retries,
                options.max_retry_delay_ms,
                options.stop_token));
            if (retry) {
                continue;
            }
            co_return stream::complete_failure(
                assistant,
                stream::stream_error(
                    "OpenAI Responses request failed with HTTP " +
                        std::to_string(response->head.status_code),
                    response->body),
                guarded_sink);
        }

        if (!started) {
            if (auto emitted = stream::emit_start(guarded_sink, assistant, started); !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        auto final_event = parser.finish();
        if (!final_event) {
            co_return stream::complete_failure(
                assistant, final_event.error(), guarded_sink);
        }
        if (*final_event) {
            if (auto processed = process_sse_event(
                    **final_event,
                    model,
                    assistant,
                    slots,
                    guarded_sink);
                !processed) {
                if (sink_failure) {
                    co_return std::unexpected(*sink_failure);
                }
                co_return stream::complete_failure(
                    assistant, processed.error(), guarded_sink);
            }
        }
        if (options.stop_token.stop_requested()) {
            co_return stream::complete_failure(
                assistant,
                util::make_error(
                    util::ErrorCode::Cancelled,
                    "Request was aborted"),
                guarded_sink);
        }
        // pi: a stream whose accumulation ends still-pending is an error.
        if (assistant.stop_reason == AssistantStopReason::Pending) {
            co_return stream::complete_failure(
                assistant,
                stream::stream_error(
                    "OpenAI Responses stream ended without a stop reason"),
                guarded_sink);
        }
        if (assistant.stop_reason == AssistantStopReason::Error) {
            co_return stream::complete_failure(
                assistant,
                stream::stream_error(assistant.error_message.value_or(
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
