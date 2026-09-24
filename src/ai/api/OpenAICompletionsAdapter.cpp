#include "OpenAICompletionsAdapter.hpp"

#include "CompletionsEvents.hpp"
#include "MessageConversion.hpp"
#include "ProviderDetection.hpp"
#include "ai/Headers.hpp"
#include "ai/Timestamps.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::api {
namespace {

[[nodiscard]] std::string completions_url(std::string_view base_url) {
    std::string result{base_url};
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    return result + "/chat/completions";
}

[[nodiscard]] support::Expected<providers::StreamRequest> build_stream_request(
        const Model& model, const AiContext& context, const ProviderStreamOptions& options) {
    auto payload = build_adapter_payload(AdapterKind::OpenAICompletions, model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = support::write_json(*payload);
    if (!body) {
        return std::unexpected(body.error());
    }

    providers::StreamRequest request;
    request.url = completions_url(model.base_url);
    request.timeout = std::chrono::milliseconds{options.timeout_ms.value_or(30000)};
    request.stop_token = options.stop_token;
    request.headers.insert(options.auth.headers.begin(), options.auth.headers.end());
    if (is_openrouter(model) && options.session_id && options.cache_retention != CacheRetention::None &&
            !has_header(request.headers, "x-session-id") && !header_deleted(options, "x-session-id")) {
        set_header(request.headers, "x-session-id", *options.session_id);
    }
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

[[nodiscard]] support::ExpectedVoid process_sse_event(const providers::SseEvent& event,
        CompletionsEventProcessor& processor,
        AssistantMessage& assistant,
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
    auto parsed = support::read_json(event.data);
    if (!parsed) {
        return std::unexpected(
                providers::make_stream_error("Malformed OpenAI Chat Completions SSE event", parsed.error().detail));
    }
    auto* object = parsed->get_if<support::JsonValue::object_t>();
    if (!object) {
        return std::unexpected(providers::make_stream_error(
                "Malformed OpenAI Chat Completions SSE event", "event data must be a JSON object"));
    }
    auto processed = processor.process(std::move(*object), assistant, sink);
    if (!processed) {
        return std::unexpected(processed.error());
    }
    if (!processed->provider_error) {
        return {};
    }
    const auto& provider = *processed->provider_error;
    inference_failure = InferenceFailure{
            .kind = provider.code ? providers::inference_failure_kind_from_provider_code(*provider.code)
                                  : InferenceFailureKind::InvalidRequest,
            .output_started = false,
            .suggested_backoff_ms = provider.suggested_backoff_ms,
            .provider_code = provider.code,
    };
    const auto code = provider.code.value_or("unknown");
    const auto message = provider.message.value_or("Unknown provider error");
    return std::unexpected(providers::make_stream_error("Error Code " + code + ": " + message));
}

} // namespace

OpenAICompletionsAdapter::OpenAICompletionsAdapter(std::shared_ptr<providers::StreamTransport> transport)
    : transport_(std::move(transport)) {}

OpenAICompletionsAdapter::OpenAICompletionsAdapter(OpenAICompletionsAdapter&&) noexcept = default;
OpenAICompletionsAdapter& OpenAICompletionsAdapter::operator=(OpenAICompletionsAdapter&&) noexcept = default;
OpenAICompletionsAdapter::~OpenAICompletionsAdapter() = default;

boost::asio::awaitable<support::Expected<AssistantMessage>> OpenAICompletionsAdapter::stream(
        const Model& model, const AiContext& context, ProviderStreamOptions options, AssistantEventSink sink) {
    if (!transport_) {
        co_return std::unexpected(
                providers::make_stream_error("OpenAI Chat Completions adapter requires a stream transport"));
    }
    if (model.api != "openai-completions") {
        co_return std::unexpected(
                providers::make_stream_error("OpenAI Chat Completions adapter received the wrong Model API"));
    }
    if (model.base_url.empty()) {
        co_return std::unexpected(providers::make_stream_error("OpenAI Chat Completions Model base URL is required"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Request was aborted"));
    }
    if ((!options.auth.api_key || options.auth.api_key->empty()) &&
            !has_header(options.auth.headers, "authorization") &&
            !has_header(options.auth.headers, "cf-aig-authorization")) {
        co_return std::unexpected(providers::make_stream_error("No API key for provider: " + model.provider));
    }

    CCH_TRY(request, build_stream_request(model, context, options));

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = current_timestamp_ms();

    struct AttemptState {
        std::unique_ptr<CompletionsEventProcessor> processor;
    };
    auto attempt_state = std::make_shared<AttemptState>();
    auto attempt_hook = [attempt_state, &model]() -> support::Expected<providers::SseEventHook> {
        attempt_state->processor = std::make_unique<CompletionsEventProcessor>(model);
        return [attempt_state](const providers::SseEvent& event,
                       AssistantMessage& assistant,
                       AssistantEventSink& sink,
                       std::optional<InferenceFailure>& inference_failure) -> support::ExpectedVoid {
            return process_sse_event(event, *attempt_state->processor, assistant, sink, inference_failure);
        };
    };
    auto finalize_hook = [attempt_state](
                                 AssistantMessage& assistant, AssistantEventSink& sink) -> support::ExpectedVoid {
        return attempt_state->processor->finish(assistant, sink);
    };

    co_return co_await providers::execute_sse_stream(providers::SseStreamExecutionOptions{
            .protocol_name = "OpenAI Chat Completions",
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
