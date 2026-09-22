#include "OpenAICodexResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ai/Headers.hpp"
#include "ai/Timestamps.hpp"
#include "ai/api/PartialJson.hpp"
#include "ai/api/ResponsesEventProcessor.hpp"
#include "ai/auth/Pkce.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CodexEvents.hpp"
#include "CodexShared.hpp"
#include "CodexWebSocketCache.hpp"

namespace cch::ai::api {
namespace {

struct WsAttemptOutcome {
    bool completed{false};
    bool output_started{false};
    bool websocket_started{false};
    support::Error error{};
    CodexFailureKind failure_kind{CodexFailureKind::Transport};
    std::string api_code{};
    std::optional<InferenceFailure> inference_failure{std::nullopt};
};

boost::asio::awaitable<support::Expected<WsAttemptOutcome>> run_ws_attempt(
    const std::shared_ptr<providers::WebSocketTransport>& ws_transport,
    const Model& model,
    const ProviderStreamOptions& options,
    const support::JsonValue& full_body,
    const providers::WebSocketConnectRequest& ws_request,
    std::optional<std::string_view> cache_session_id,
    std::string_view account_id,
    CodexWebSocketCache& cache,
    AssistantMessage& assistant,
    bool started,
    AssistantEventSink& sink) {
    const auto started_state = std::make_shared<bool>(started);
    const auto websocket_started_state = std::make_shared<bool>(false);

    const auto finish_failed = [started_state, websocket_started_state](
            support::Error error,
            CodexFailureKind kind,
            std::string api_code = {},
            std::optional<InferenceFailure> inference_failure = std::nullopt) {
        if (!inference_failure) {
            inference_failure = InferenceFailure{
                    .kind = kind == CodexFailureKind::Cancelled
                        ? InferenceFailureKind::Cancelled
                        : kind == CodexFailureKind::Transport
                            ? InferenceFailureKind::TransientTransportFailure
                            : InferenceFailureKind::InvalidRequest,
                    .output_started = *started_state,
                    .provider_code = api_code.empty()
                        ? std::nullopt
                        : std::optional<std::string>{api_code},
            };
        } else {
            inference_failure->output_started = *started_state;
        }
        return WsAttemptOutcome{
                .completed = false,
                .output_started = *started_state,
                .websocket_started = *websocket_started_state,
                .error = std::move(error),
                .failure_kind = kind,
                .api_code = std::move(api_code),
                .inference_failure = std::move(inference_failure),
        };
    };

    auto entry = cache.try_reuse(cache_session_id, account_id);
    std::shared_ptr<providers::WebSocket> socket;
    if (!entry) {
        auto connected = co_await ws_transport->async_connect(ws_request);
        if (!connected) {
            const auto kind = connected.error().code == support::ErrorCode::Cancelled
                ? CodexFailureKind::Cancelled
                : CodexFailureKind::Transport;
            co_return finish_failed(connected.error(), kind);
        }
        auto acquisition = cache.register_or_reuse(
            cache_session_id, account_id, std::move(*connected));
        entry = acquisition.entry;
        socket = std::move(acquisition.socket);
    } else {
        socket = entry->socket;
    }
    const auto release_socket = [&](bool keep) {
        if (entry) {
            cache.release(entry, keep);
        } else {
            socket->close();
        }
    };

    support::JsonValue request_body = full_body;
    if (entry && entry->continuation) {
        auto delta = cached_input_delta(full_body, *entry->continuation);
        if (!delta || entry->continuation->last_response_id.empty()) {
            entry->continuation.reset();
        } else {
            auto object = *full_body.get_if<JsonObject>();
            object.insert_or_assign("input", support::JsonValue{std::move(*delta)});
            object.insert_or_assign(
                "previous_response_id", entry->continuation->last_response_id);
            request_body = support::JsonValue{std::move(object)};
        }
    }
    auto frame = ws_frame_json(request_body);
    if (!frame) {
        release_socket(false);
        co_return finish_failed(frame.error(), CodexFailureKind::Transport);
    }

    auto sent = co_await socket->async_send(*frame);
    if (!sent) {
        const auto kind = sent.error().code == support::ErrorCode::Cancelled
            ? CodexFailureKind::Cancelled
            : CodexFailureKind::Transport;
        release_socket(false);
        co_return finish_failed(sent.error(), kind);
    }

    ResponsesEventProcessor processor{ResponsesDialect::Codex, ResponsesDelivery::WebSocket, model};
    CodexFailure failure;
    for (;;) {
        if (options.stop_token.stop_requested()) {
            release_socket(false);
            co_return finish_failed(
                support::make_error(
                    support::ErrorCode::Cancelled, "Request was aborted"),
                CodexFailureKind::Cancelled);
        }
        auto received = co_await socket->async_receive();
        if (!received) {
            const auto kind = received.error().code == support::ErrorCode::Cancelled
                ? CodexFailureKind::Cancelled
                : CodexFailureKind::Transport;
            release_socket(false);
            co_return finish_failed(received.error(), kind);
        }
        if (!*received) {
            release_socket(false);
            co_return finish_failed(providers::make_stream_error("WebSocket stream closed before response.completed"),
                    CodexFailureKind::Transport);
        }
        auto parsed = support::read_json(**received);
        if (!parsed) {
            failure = CodexFailure{
                    .kind = CodexFailureKind::Protocol,
                    .code = {},
                    .message = {},
                    .error = providers::make_stream_error("Invalid Codex WebSocket JSON: " + parsed.error().detail),
            };
            release_socket(false);
            co_return finish_failed(failure.error, CodexFailureKind::Protocol);
        }
        auto* event = parsed->get_if<JsonObject>();
        if (!event) {
            // pi skips frames whose type is not a string.
            continue;
        }
        const auto type_found = event->find("type");
        const auto* frame_type = type_found != event->end() ? type_found->second.get_if<std::string>() : nullptr;
        const bool api_error_event = frame_type &&
            (*frame_type == "error" || *frame_type == "response.failed");
        if (!api_error_event && !*websocket_started_state) {
            *websocket_started_state = true;
            if (!*started_state) {
                if (auto emitted = providers::emit_start(sink, assistant, *started_state); !emitted) {
                    release_socket(false);
                    co_return std::unexpected(emitted.error());
                }
            }
        }
        auto action = process_codex_json_event(std::move(*event), processor, assistant, sink, &failure);
        if (!action) {
            const auto kind = failure.kind;
            release_socket(false);
            co_return finish_failed(action.error(), kind, failure.code);
        }
        if (*action == WsFrameAction::Terminal) {
            break;
        }
    }

    if (options.stop_token.stop_requested()) {
        release_socket(false);
        co_return finish_failed(
            support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
            CodexFailureKind::Cancelled);
    }
    if (assistant.stop_reason == AssistantStopReason::Error) {
        // pi's assertSuccessfulOutput throws here; the outer loop treats it as
        // a started transport failure (diagnostic + no SSE fallback).
        release_socket(false);
        co_return finish_failed(providers::make_stream_error(assistant.error_message.value_or("Codex request failed")),
                CodexFailureKind::Transport);
    }
    if (entry && assistant.response_id) {
        auto items = build_responses_continuation_items(model, assistant);
        if (items) {
            entry->continuation = CodexContinuation{
                .last_request_body = full_body,
                .last_response_id = *assistant.response_id,
                .last_response_items = std::move(*items),
            };
        }
    }
    release_socket(true);
    co_return WsAttemptOutcome{
            .completed = true,
            .output_started = *started_state,
            .websocket_started = *websocket_started_state,
    };
}

/// The completed-WS-attempt terminal: cancelled, assistant-level error, or
/// the success Done event.

/// pi codex websocket retry policy: a previous_response_not_found miss and a
/// pre-start connection-limit rejection each retry exactly once.
[[nodiscard]] bool should_retry_ws_failure(const WsAttemptOutcome& outcome,
        bool websocket_started,
        bool& retried_previous_response,
        bool& retried_connection_limit) {
    const bool connection_limit_before_start = !websocket_started && outcome.failure_kind == CodexFailureKind::Api &&
                                               outcome.api_code == kWebSocketConnectionLimitReached;
    const bool previous_response_not_found =
            outcome.failure_kind == CodexFailureKind::Api && outcome.api_code == kPreviousResponseNotFound;
    const bool aborted = outcome.failure_kind == CodexFailureKind::Cancelled;
    if (!aborted && previous_response_not_found && !retried_previous_response) {
        retried_previous_response = true;
        return true;
    }
    if (!aborted && connection_limit_before_start && !retried_connection_limit) {
        retried_connection_limit = true;
        return true;
    }
    return false;
}

} // namespace

struct OpenAICodexResponsesAdapter::Impl {
    explicit Impl(providers::CodexWebSocketCacheConfig config)
        : cache(std::move(config)) {}

    CodexWebSocketCache cache;
    std::set<std::string, std::less<>> sse_fallback_sessions;
};

OpenAICodexResponsesAdapter::OpenAICodexResponsesAdapter(
    std::shared_ptr<providers::StreamTransport> http_transport,
    std::shared_ptr<providers::WebSocketTransport> ws_transport,
    providers::CodexWebSocketCacheConfig cache_config)
    : http_transport_(std::move(http_transport)),
      ws_transport_(std::move(ws_transport)),
      impl_(std::make_unique<Impl>(cache_config)) {}

OpenAICodexResponsesAdapter::OpenAICodexResponsesAdapter(OpenAICodexResponsesAdapter&&) noexcept = default;
OpenAICodexResponsesAdapter& OpenAICodexResponsesAdapter::operator=(OpenAICodexResponsesAdapter&&) noexcept = default;
OpenAICodexResponsesAdapter::~OpenAICodexResponsesAdapter() = default;

boost::asio::awaitable<support::Expected<AssistantMessage>> OpenAICodexResponsesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!http_transport_ || !ws_transport_) {
        co_return std::unexpected(
                providers::make_stream_error("Codex Responses adapter requires HTTP and WebSocket transports"));
    }
    if (model.api != "openai-codex-responses") {
        co_return std::unexpected(providers::make_stream_error("Codex Responses adapter received the wrong Model API"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (!options.auth.api_key || options.auth.api_key->empty()) {
        co_return std::unexpected(providers::make_stream_error("No API key for provider: " + model.provider));
    }

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = current_timestamp_ms();

    std::optional<support::Error> sink_failure;
    AssistantEventSink guarded_sink =
        [&sink, &sink_failure](const AssistantStreamEvent& event) -> support::ExpectedVoid {
            auto emitted = providers::emit(sink, event);
            if (!emitted) {
                sink_failure = emitted.error();
            }
            return emitted;
        };

    CCH_TRY(account_id, extract_account_id(*options.auth.api_key));
    CCH_TRY(payload, build_adapter_payload(
        AdapterKind::OpenAICodexResponses, model, context, options));
    CCH_TRY(body_json, support::write_json(payload));
    const auto codex_url = resolve_codex_url(model.base_url);
    const auto cache_session_id = options.session_id;

    const auto ws_headers = codex_headers(options, account_id, true);
    const auto sse_headers = codex_headers(options, account_id, false);
    providers::WebSocketConnectRequest ws_request;
    ws_request.url = resolve_codex_websocket_url(model.base_url);
    ws_request.headers = ws_headers;
    ws_request.connect_timeout = kDefaultWebSocketConnectTimeout;
    if (options.timeout_ms) {
        ws_request.idle_timeout = std::chrono::milliseconds{*options.timeout_ms};
    }
    ws_request.stop_token = options.stop_token;

    providers::StreamRequest sse_request;
    sse_request.url = codex_url;
    sse_request.timeout = std::chrono::milliseconds{
        options.timeout_ms.value_or(30000)};
    sse_request.stop_token = options.stop_token;
    sse_request.headers.insert(sse_headers.begin(), sse_headers.end());
    sse_request.body = body_json;

    CodexWebSocketCache& cache = impl_->cache;
    auto& sse_fallback_sessions = impl_->sse_fallback_sessions;

    bool started = false;
    const bool ws_disabled = cache_session_id.has_value() &&
        sse_fallback_sessions.contains(std::string{*cache_session_id});

    if (!ws_disabled) {
        bool retried_previous_response = false;
        bool retried_connection_limit = false;
        while (true) {
            CCH_TRY(outcome,
                    co_await run_ws_attempt(ws_transport_,
                            model,
                            options,
                            payload,
                            ws_request,
                            cache_session_id,
                            account_id,
                            cache,
                            assistant,
                            started,
                            guarded_sink));
            started = outcome.output_started;
            const bool websocket_started = outcome.websocket_started;
            if (outcome.completed) {
                co_return co_await finish_ws_completed(std::move(assistant), started, options.stop_token, guarded_sink);
            }

            if (should_retry_ws_failure(
                        outcome, websocket_started, retried_previous_response, retried_connection_limit) &&
                    !options.stop_token.stop_requested()) {
                continue;
            }
            const bool aborted =
                    options.stop_token.stop_requested() || outcome.failure_kind == CodexFailureKind::Cancelled;
            const bool connection_limit_before_start = !websocket_started &&
                                                       outcome.failure_kind == CodexFailureKind::Api &&
                                                       outcome.api_code == kWebSocketConnectionLimitReached;
            const bool terminal_api = aborted || ((outcome.failure_kind == CodexFailureKind::Api ||
                                                          outcome.failure_kind == CodexFailureKind::Protocol) &&
                                                         !connection_limit_before_start);
            if (!terminal_api) {
                // Transport failures carry the diagnostic and, without a
                // session, fall through to the SSE path below.
                append_transport_diagnostic(assistant, outcome.error, "auto", websocket_started, body_json.size());
                if (cache_session_id) {
                    sse_fallback_sessions.insert(std::string{*cache_session_id});
                }
            }
            if (terminal_api || websocket_started) {
                co_return complete_failure(assistant, outcome.error, guarded_sink, outcome.inference_failure, started);
            }
            break;
        }
    }

    // SSE fallback path: pi's plain-JSON branch (zstd compression omitted).
    struct AttemptState {
        std::unique_ptr<ResponsesEventProcessor> processor;
    };

    auto attempt_state = std::make_shared<AttemptState>();

    auto attempt_hook = [attempt_state, &model]() -> support::Expected<providers::SseEventHook> {
        attempt_state->processor =
                std::make_unique<ResponsesEventProcessor>(ResponsesDialect::Codex, ResponsesDelivery::Sse, model);
        return [attempt_state](const providers::SseEvent& event,
                       AssistantMessage& assistant,
                       AssistantEventSink& sink,
                       std::optional<InferenceFailure>& inference_failure) -> support::ExpectedVoid {
            return process_codex_sse_event(
                event,
                *attempt_state->processor,
                assistant,
                sink,
                inference_failure);
        };
    };

    auto finalize_hook = [attempt_state](AssistantMessage& assistant, AssistantEventSink&) -> support::ExpectedVoid {
        return attempt_state->processor->finish(assistant);
    };

    co_return co_await providers::execute_sse_stream(providers::SseStreamExecutionOptions{
        .protocol_name = "Codex",
        .request = sse_request,
        .transport = *http_transport_,
        .options = options,
        .initial_assistant = std::move(assistant),
        .sink = std::move(sink),
        .attempt_hook = std::move(attempt_hook),
        .finalize_hook = std::move(finalize_hook),
    });
}

} // namespace cch::ai::api
