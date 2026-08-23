#include "OpenAIResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ai/api/ResponsesEventProcessor.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::api {
namespace {

using JsonObject = support::JsonValue::object_t;

[[nodiscard]] TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

[[nodiscard]] bool header_name_equal(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char left_character, char right_character) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        };
        return lower(left_character) == lower(right_character);
    });
}

template <typename Headers> void set_header(Headers& headers, std::string name, std::string value) {
    std::erase_if(headers, [&name](const auto& header) { return header_name_equal(header.first, name); });
    headers.emplace(std::move(name), std::move(value));
}

template <typename Headers> [[nodiscard]] bool has_header(const Headers& headers, std::string_view name) {
    return std::ranges::any_of(headers,
            [name](const auto& header) { return header_name_equal(header.first, name) && !header.second.empty(); });
}

[[nodiscard]] bool header_deleted(const ProviderStreamOptions& options, std::string_view name) {
    return std::ranges::any_of(
            options.deleted_headers, [name](const auto& header) { return header_name_equal(header, name); });
}

[[nodiscard]] support::Error stream_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Stream,
            providers::bounded_provider_error_detail(std::move(message)),
            providers::bounded_provider_error_detail(std::move(detail)));
}

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
    if (options.auth.api_key && !has_header(request.headers, "authorization") &&
            !header_deleted(options, "authorization")) {
        set_header(request.headers, "Authorization", "Bearer " + *options.auth.api_key);
    }
    if (!has_header(request.headers, "content-type") && !header_deleted(options, "content-type")) {
        set_header(request.headers, "Content-Type", "application/json");
    }
    if (!has_header(request.headers, "accept") && !header_deleted(options, "accept")) {
        set_header(request.headers, "Accept", "text/event-stream");
    }
    request.body = std::move(*body);
    return request;
}

[[nodiscard]] support::ExpectedVoid process_json_event(
        ResponsesEventProcessor& processor, JsonObject event, AssistantMessage& assistant, AssistantEventSink& sink) {
    const auto type_found = event.find("type");
    const auto* type = type_found != event.end() ? type_found->second.get_if<std::string>() : nullptr;
    const bool response_failed = type && *type == "response.failed";
    auto processed = processor.process(std::move(event), assistant, sink);
    if (!processed) {
        return std::unexpected(processed.error());
    }
    if (!processed->provider_error) {
        return {};
    }

    const auto& provider_error = *processed->provider_error;
    if (response_failed) {
        std::string detail = provider_error.code.value_or("unknown");
        detail += ": ";
        detail += provider_error.message.value_or("no message");
        return std::unexpected(stream_error(std::move(detail)));
    }
    const auto code = provider_error.code.value_or("unknown");
    const auto message = provider_error.message.value_or("Unknown error");
    return std::unexpected(stream_error("Error Code " + std::string{code} + ": " + std::string{message}));
}

[[nodiscard]] support::ExpectedVoid process_sse_event(const providers::SseEvent& event,
        ResponsesEventProcessor& processor,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream_error(event.data));
    }
    auto parsed = support::read_json(event.data);
    if (!parsed) {
        if (event.event != "message" && !event.event.starts_with("response.")) {
            return {};
        }
        return std::unexpected(stream_error("Malformed OpenAI Responses SSE event", parsed.error().detail));
    }
    auto* event_object = parsed->get_if<JsonObject>();
    if (!event_object) {
        return std::unexpected(
                stream_error("Malformed OpenAI Responses SSE event", "event data must be a JSON object"));
    }
    return process_json_event(processor, std::move(*event_object), assistant, sink);
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
        co_return std::unexpected(stream_error("OpenAI Responses adapter requires a stream transport"));
    }
    if (model.api != "openai-responses") {
        co_return std::unexpected(stream_error("OpenAI Responses adapter received the wrong Model API"));
    }
    if (model.base_url.empty()) {
        co_return std::unexpected(stream_error("OpenAI Responses Model base URL is required"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if ((!options.auth.api_key || options.auth.api_key->empty()) &&
            !has_header(options.auth.headers, "authorization") &&
            !has_header(options.auth.headers, "cf-aig-authorization")) {
        co_return std::unexpected(stream_error("No API key for provider: " + model.provider));
    }

    CCH_TRY(request, build_stream_request(model, context, options));

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = current_timestamp_ms();

    struct AttemptState {
        std::unique_ptr<ResponsesEventProcessor> processor;
    };

    auto attempt_state = std::make_shared<AttemptState>();

    auto attempt_hook = [attempt_state, &model]() -> support::Expected<providers::SseEventHook> {
        attempt_state->processor =
                std::make_unique<ResponsesEventProcessor>(ResponsesDialect::DeepSeek, ResponsesDelivery::Sse, model);
        return [attempt_state](const providers::SseEvent& event,
                       AssistantMessage& assistant,
                       AssistantEventSink& sink) -> support::ExpectedVoid {
            return process_sse_event(event, *attempt_state->processor, assistant, sink);
        };
    };

    auto finalize_hook = [attempt_state](AssistantMessage& assistant) -> support::ExpectedVoid {
        return attempt_state->processor->finish(assistant);
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
