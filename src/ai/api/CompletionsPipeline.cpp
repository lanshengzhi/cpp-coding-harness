#include "CompletionsPipeline.hpp"

#include "CompletionsEvents.hpp"
#include "ai/api/CompletionsPayload.hpp"
#include "support/CancellationBridge.hpp"
#include "ai/Headers.hpp"
#include "ai/SimpleOptions.hpp"
#include "ai/api/RequestHeaders.hpp"
#include <cch/ai/InferenceFailure.hpp>
#include <cch/support/SseParser.hpp>
#include <cch/ai/Timestamps.hpp>
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
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

[[nodiscard]] ModelThinkingLevel to_model_thinking_level(ThinkingLevel level) {
    switch (level) {
    case ThinkingLevel::Minimal:
        return ModelThinkingLevel::Minimal;
    case ThinkingLevel::Low:
        return ModelThinkingLevel::Low;
    case ThinkingLevel::Medium:
        return ModelThinkingLevel::Medium;
    case ThinkingLevel::High:
        return ModelThinkingLevel::High;
    case ThinkingLevel::XHigh:
        return ModelThinkingLevel::XHigh;
    case ThinkingLevel::Max:
        return ModelThinkingLevel::Max;
    }
    return ModelThinkingLevel::Medium;
}

/// The Models-level request preparation, applied once inside the adapter: the
/// caller's `SimpleStreamOptions` resolve against the Model and AiContext
/// (pi `resolveStreamOptions`), the trusted authentication assembles from the
/// gateway key and the model's own header set, and the request header list
/// resolves from the authentication headers plus the caller's overrides.
[[nodiscard]] PreparedStreamOptions prepare_stream_options(
        const Model& model, const AiContext& context, RequestAuth auth, SimpleStreamOptions options) {
    if (model.headers) {
        merge_headers(auth.headers, *model.headers);
        erase_header(auth.headers, "authorization");
    }
    auto request_headers = resolve_stream_request_headers(auth.headers, std::move(options.headers));

    const auto request_env = std::move(options.env);
    const auto cache_retention = detail::resolve_cache_retention(options.cache_retention, request_env);
    const auto request_session_id =
            cache_retention == CacheRetention::None ? std::optional<std::string>{} : options.session_id;

    const auto reasoning = options.reasoning ? std::optional<ModelThinkingLevel>{clamp_thinking_level(
                                                       model, to_model_thinking_level(*options.reasoning))}
                                             : std::nullopt;
    const auto requested_max_tokens = options.max_tokens.value_or(model.max_tokens);
    const auto max_tokens = detail::clamp_max_tokens_to_context(model, context, requested_max_tokens);
    return PreparedStreamOptions{
            .auth = std::move(auth),
            .headers = std::move(request_headers),
            .temperature = options.temperature,
            .max_tokens = max_tokens,
            .reasoning = reasoning,
            .session_id = request_session_id,
            .cache_retention = cache_retention,
            .timeout_ms = options.timeout_ms,
            .max_retries = options.max_retries,
            .max_retry_delay_ms = options.max_retry_delay_ms.value_or(60000),
            .stop_token = options.stop_token,
    };
}

} // namespace

support::Expected<support::StreamRequest> build_stream_request(
        std::string_view base_url, const Model& model, const AiContext& context, const PreparedStreamOptions& options) {
    const auto& auth = options.auth;
    auto payload = build_completions_payload(model, context, options);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto body = support::write_json(*payload);
    if (!body) {
        return std::unexpected(body.error());
    }

    support::StreamRequest request;
    request.url = completions_url(base_url);
    request.timeout = std::chrono::milliseconds{options.timeout_ms.value_or(30000)};
    request.stop_token = options.stop_token;
    for (const auto& [name, value] : options.headers) {
        if (value) {
            request.headers.emplace(name, *value);
        }
    }
    // The gateway's session identity header (ADR 0074): first choice in
    // Magpie's session-affinity resolution.
    if (options.session_id && !has_header(request.headers, "x-magpie-session") &&
            !has_header_tombstone(options.headers, "x-magpie-session")) {
        set_header(request.headers, "X-Magpie-Session", *options.session_id);
    }
    if (auth.api_key && !auth.api_key->empty()) {
        set_header(request.headers, "Authorization", "Bearer " + *auth.api_key);
    }
    if (!has_header(request.headers, "content-type") && !has_header_tombstone(options.headers, "content-type")) {
        set_header(request.headers, "Content-Type", "application/json");
    }
    if (!has_header(request.headers, "accept") && !has_header_tombstone(options.headers, "accept")) {
        set_header(request.headers, "Accept", "text/event-stream");
    }
    request.body = std::move(*body);
    return request;
}

namespace {

[[nodiscard]] support::ExpectedVoid process_sse_event(const support::SseEvent& event,
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

[[nodiscard]] support::Error normalize_transport_error(std::string_view protocol_name, const support::Error& error) {
    if (error.code == support::ErrorCode::Cancelled) {
        return support::make_error(support::ErrorCode::Cancelled, std::string{kDefaultAbortDiagnostic});
    }
    if (error.code == support::ErrorCode::Unknown) {
        return error;
    }
    return providers::make_stream_error(
            error.message.empty() ? std::format("{} request failed", protocol_name) : error.message, error.detail);
}

[[nodiscard]] providers::ProviderFailure response_failure(const support::StreamResponse& response) {
    HeaderValues headers;
    headers.insert(response.head.headers.begin(), response.head.headers.end());
    const auto provider_code = providers::provider_error_code_from_payload(response.body);
    auto failure = providers::ProviderFailure{
            .network_error = false,
            .status = response.head.status_code,
            .headers = std::move(headers),
            .message = response.body,
            .inference_failure =
                    InferenceFailure{
                            .kind = provider_code ? providers::inference_failure_kind_from_provider_code(*provider_code)
                                                  : providers::inference_failure_kind_from_http_status(
                                                            response.head.status_code),
                            .output_started = false,
                            .suggested_backoff_ms = std::nullopt,
                            .provider_code = provider_code,
                    },
    };
    failure.inference_failure->suggested_backoff_ms =
            providers::provider_backoff_hint_ms(failure, current_timestamp_ms());
    return failure;
}

[[nodiscard]] providers::ProviderFailure transport_failure(const support::Error& error) {
    return providers::ProviderFailure{
            .network_error = error.code == support::ErrorCode::Network || error.code == support::ErrorCode::Timeout,
            .status = std::nullopt,
            .headers = {},
            .message = error.detail.empty() ? error.message : error.detail,
            .inference_failure =
                    InferenceFailure{
                            .kind = providers::inference_failure_kind_from_transport(error.code),
                            .output_started = false,
                            .suggested_backoff_ms = std::nullopt,
                            .provider_code = std::nullopt,
                    },
    };
}

[[nodiscard]] boost::asio::awaitable<support::Expected<bool>> retry_provider_failure(
        const providers::ProviderFailure& failure, std::uint32_t attempt, const PreparedStreamOptions& options) {
    if (attempt >= options.max_retries || !providers::is_retryable_provider_failure(failure)) {
        co_return false;
    }
    CCH_TRY(delay,
            providers::provider_retry_delay_ms(failure, attempt, options.max_retry_delay_ms, current_timestamp_ms()));
    if (delay > 0) {
        CCH_TRY_VOID(co_await support::interruptible_sleep(
                std::chrono::milliseconds{delay}, options.stop_token, std::string{kDefaultAbortDiagnostic}));
    }
    co_return true;
}

class CompletionsExecutionSession final {
public:
    CompletionsExecutionSession(Model model,
            support::StreamRequest request,
            support::StreamTransport& transport,
            const PreparedStreamOptions& options,
            providers::StreamEmitter& emitter,
            bool& request_started)
        : model_(std::move(model)), request_(std::move(request)), transport_(transport), options_(options),
          emitter_(emitter), request_started_(request_started) {}

    [[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> execute() {
        for (std::uint32_t attempt = 0;; ++attempt) {
            if (options_.stop_token.stop_requested()) {
                co_return emitter_.finalize_cancelled();
            }

            CompletionsEventProcessor processor(model_);
            support::SseParser parser;
            std::optional<support::Error> handler_failure;
            std::optional<InferenceFailure> inference_failure;

            AssistantEventSink session_sink = [this](const AssistantStreamEvent& event) -> support::ExpectedVoid {
                return emitter_.emit_event(event);
            };

            auto handle_chunk = [this, &processor, &parser, &handler_failure, &inference_failure, &session_sink](
                                        std::string_view bytes) -> support::ExpectedVoid {
                if (auto emitted = emitter_.emit_start(); !emitted) {
                    handler_failure = emitted.error();
                    return std::unexpected(emitted.error());
                }
                auto events = parser.append(bytes);
                if (!events) {
                    handler_failure = events.error();
                    return std::unexpected(events.error());
                }
                for (const auto& event : *events) {
                    if (auto processed = process_sse_event(
                                event, processor, emitter_.assistant(), session_sink, inference_failure);
                            !processed) {
                        handler_failure = processed.error();
                        return std::unexpected(processed.error());
                    }
                }
                return {};
            };

            request_started_ = true;
            if (options_.auth.api_key && !options_.auth.api_key->empty()) {
                set_header(request_.headers, "Authorization", "Bearer " + *options_.auth.api_key);
            }
            auto response = co_await transport_.async_stream(request_, handle_chunk);
            if (!response) {
                if (emitter_.sink_broken()) {
                    co_return std::unexpected(*emitter_.sink_failure());
                }
                if (handler_failure) {
                    co_return emitter_.finalize_failure(
                            *handler_failure, std::move(inference_failure), AssistantStopReason::Error);
                }
                if (response.error().code == support::ErrorCode::Cancelled || options_.stop_token.stop_requested()) {
                    co_return emitter_.finalize_cancelled();
                }
                const auto failure = transport_failure(response.error());
                if (!emitter_.output_started()) {
                    auto retry = co_await retry_provider_failure(failure, attempt, options_);
                    if (!retry) {
                        if (retry.error().code == support::ErrorCode::Cancelled ||
                                options_.stop_token.stop_requested()) {
                            co_return emitter_.finalize_cancelled();
                        }
                        co_return emitter_.finalize_failure(
                                retry.error(), failure.inference_failure, AssistantStopReason::Error);
                    }
                    if (*retry) {
                        continue;
                    }
                }
                co_return emitter_.finalize_failure(
                        normalize_transport_error("OpenAI Chat Completions", response.error()),
                        failure.inference_failure,
                        AssistantStopReason::Error);
            }

            if (response->head.status_code < 200 || response->head.status_code >= 300) {
                const auto failure = response_failure(*response);
                if (!emitter_.output_started()) {
                    auto retry = co_await retry_provider_failure(failure, attempt, options_);
                    if (!retry) {
                        if (retry.error().code == support::ErrorCode::Cancelled ||
                                options_.stop_token.stop_requested()) {
                            co_return emitter_.finalize_cancelled();
                        }
                        co_return emitter_.finalize_failure(
                                retry.error(), failure.inference_failure, AssistantStopReason::Error);
                    }
                    if (*retry) {
                        continue;
                    }
                }
                co_return emitter_.finalize_failure(
                        providers::make_stream_error(std::format("OpenAI Chat Completions request failed with HTTP {}",
                                                             response->head.status_code),
                                response->body),
                        failure.inference_failure,
                        AssistantStopReason::Error);
            }

            if (auto emitted = emitter_.emit_start(); !emitted) {
                co_return std::unexpected(emitted.error());
            }

            auto final_event = parser.finish();
            if (!final_event) {
                co_return emitter_.finalize_failure(
                        final_event.error(), std::move(inference_failure), AssistantStopReason::Error);
            }
            if (*final_event) {
                if (auto processed = process_sse_event(
                            **final_event, processor, emitter_.assistant(), session_sink, inference_failure);
                        !processed) {
                    if (emitter_.sink_broken()) {
                        co_return std::unexpected(*emitter_.sink_failure());
                    }
                    co_return emitter_.finalize_failure(
                            processed.error(), std::move(inference_failure), AssistantStopReason::Error);
                }
            }
            if (options_.stop_token.stop_requested()) {
                co_return emitter_.finalize_cancelled();
            }
            if (auto finalized = processor.finish(emitter_.assistant(), session_sink); !finalized) {
                if (emitter_.sink_broken()) {
                    co_return std::unexpected(*emitter_.sink_failure());
                }
                co_return emitter_.finalize_failure(
                        finalized.error(), std::move(inference_failure), AssistantStopReason::Error);
            }
            if (emitter_.assistant().stop_reason == AssistantStopReason::Error) {
                co_return emitter_.finalize_failure(
                        providers::make_stream_error(
                                emitter_.assistant().error_message.value_or("OpenAI Chat Completions request failed")),
                        std::move(inference_failure),
                        AssistantStopReason::Error);
            }
            if (emitter_.assistant().stop_reason == AssistantStopReason::Aborted) {
                auto terminal = emitter_.assistant();
                co_return emitter_.finalize_error_event(AssistantErrorEvent{
                        .reason = terminal.stop_reason,
                        .error = std::move(terminal),
                        .failure = support::make_error(support::ErrorCode::Cancelled, std::string{}),
                        .inference_failure = std::move(inference_failure),
                });
            }
            co_return emitter_.finalize_success(emitter_.assistant().stop_reason);
        }
    }

private:
    Model model_;
    support::StreamRequest request_;
    support::StreamTransport& transport_;
    const PreparedStreamOptions& options_;
    providers::StreamEmitter& emitter_;
    bool& request_started_; // retained Models outlives this execution session.
};

} // namespace

boost::asio::awaitable<support::Expected<AssistantMessage>> execute_completions_stream(
        support::StreamTransport& transport,
        std::string base_url,
        const Model& model,
        const AiContext& context,
        RequestAuth auth,
        SimpleStreamOptions options,
        providers::StreamEmitter& emitter,
        bool& request_started) {
    if (model.api != kOpenAICompletionsApi) {
        co_return std::unexpected(
                providers::make_stream_error("OpenAI Chat Completions adapter received the wrong Model API"));
    }
    if (base_url.empty()) {
        co_return std::unexpected(providers::make_stream_error("OpenAI Chat Completions gateway URL is required"));
    }

    const auto prepared = prepare_stream_options(model, context, std::move(auth), std::move(options));

    CCH_TRY(request, build_stream_request(base_url, model, context, prepared));
    if ((!prepared.auth.api_key || prepared.auth.api_key->empty()) && !has_header(request.headers, "authorization") &&
            !has_header(request.headers, "cf-aig-authorization")) {
        co_return std::unexpected(providers::make_stream_error("No API key for provider: " + model.provider));
    }

    CompletionsExecutionSession session(model, std::move(request), transport, prepared, emitter, request_started);
    co_return co_await session.execute();
}

} // namespace cch::ai::api
