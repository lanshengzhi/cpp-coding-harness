#include "StreamExecutionEngine.hpp"

#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

namespace cch::ai::providers {
namespace {

[[nodiscard]] TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] support::Error stream_error(std::string message, std::string detail = {}) {
    return support::make_error(
        support::ErrorCode::Stream,
        bounded_provider_error_detail(std::move(message)),
        bounded_provider_error_detail(std::move(detail)));
}

[[nodiscard]] support::Error normalize_transport_error(
    std::string_view protocol_name,
    const support::Error& error) {
    if (error.code == support::ErrorCode::Cancelled) {
        return support::make_error(support::ErrorCode::Cancelled, "Request was aborted");
    }
    if (error.code == support::ErrorCode::Unknown) {
        return error;
    }
    return stream_error(
        error.message.empty()
            ? std::format("{} request failed", protocol_name)
            : error.message,
        error.detail);
}

void ensure_tool_arguments_allocated(AssistantMessage& assistant) {
    for (auto& content : assistant.content) {
        auto* tool = std::get_if<ToolCallContent>(&content);
        if (tool && !tool->arguments) {
            tool->arguments = support::JsonValue{support::JsonValue::object_t{}};
            tool->arguments_valid = true;
            tool->argument_error = std::nullopt;
        }
    }
}

[[nodiscard]] support::Expected<AssistantMessage> complete_failure(
    AssistantMessage assistant,
    support::Error failure,
    AssistantEventSink& sink) {
    ensure_tool_arguments_allocated(assistant);
    const auto aborted = failure.code == support::ErrorCode::Cancelled;
    assistant.stop_reason = aborted
        ? AssistantStopReason::Aborted
        : AssistantStopReason::Error;
    if (aborted) {
        assistant.error_message = "Request was aborted";
        failure = support::make_error(support::ErrorCode::Cancelled, *assistant.error_message);
    } else {
        std::string diagnostic = failure.message;
        if (!failure.detail.empty() && diagnostic.find(failure.detail) == std::string::npos) {
            if (!diagnostic.empty()) {
                diagnostic += ": ";
            }
            diagnostic += failure.detail;
        }
        assistant.error_message = bounded_provider_error_detail(std::move(diagnostic));
        failure = support::make_error(support::ErrorCode::Stream, *assistant.error_message);
    }
    auto emitted = emit(sink, AssistantErrorEvent{
        .reason = assistant.stop_reason,
        .error = assistant,
        .failure = std::move(failure),
    });
    if (!emitted) {
        return std::unexpected(emitted.error());
    }
    return assistant;
}

[[nodiscard]] ProviderFailure response_failure(const StreamResponse& response) {
    ProviderHeaders headers;
    headers.insert(response.head.headers.begin(), response.head.headers.end());
    return ProviderFailure{
        .network_error = false,
        .status = response.head.status_code,
        .headers = std::move(headers),
        .message = response.body,
    };
}

[[nodiscard]] ProviderFailure transport_failure(const support::Error& error) {
    return ProviderFailure{
        .network_error = error.code == support::ErrorCode::Network ||
                         error.code == support::ErrorCode::Timeout,
        .status = std::nullopt,
        .headers = {},
        .message = error.detail.empty() ? error.message : error.detail,
    };
}

[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> wait_before_retry(
    std::uint64_t delay_ms,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (delay_ms == 0) {
        co_return support::ExpectedVoid{};
    }
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, std::chrono::milliseconds{delay_ms});
    std::stop_callback cancellation{stop_token, [&timer] { timer.cancel(); }};
    boost::system::error_code error;
    co_await timer.async_wait(boost::asio::redirect_error(
        boost::asio::use_awaitable, error));
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (error) {
        co_return std::unexpected(stream_error(
            "Retry wait failed",
            error.message()));
    }
    co_return support::ExpectedVoid{};
}

[[nodiscard]] boost::asio::awaitable<support::Expected<bool>> retry_provider_failure(
    ProviderFailure failure,
    std::uint32_t attempt,
    const ProviderStreamOptions& options) {
    if (attempt >= options.max_retries ||
        !is_retryable_provider_failure(failure)) {
        co_return false;
    }
    CCH_TRY(delay, provider_retry_delay_ms(
        failure,
        attempt,
        options.max_retry_delay_ms,
        current_timestamp_ms()));
    CCH_TRY_VOID(co_await wait_before_retry(delay, options.stop_token));
    co_return true;
}

[[nodiscard]] support::ExpectedVoid emit_start(
    AssistantEventSink& sink,
    AssistantMessage& assistant,
    bool& started) {
    if (started) {
        return {};
    }
    started = true;
    return emit(sink, AssistantStartEvent{.partial = assistant});
}

} // namespace

boost::asio::awaitable<support::Expected<AssistantMessage>>
execute_sse_stream(SseStreamExecutionOptions execution_options) {
    auto& request = execution_options.request;
    auto& transport = execution_options.transport;
    const auto& options = execution_options.options;
    auto assistant = std::move(execution_options.initial_assistant);
    auto& sink = execution_options.sink;
    auto& attempt_hook = execution_options.attempt_hook;
    auto& finalize_hook = execution_options.finalize_hook;
    const auto protocol_name = execution_options.protocol_name;

    std::optional<support::Error> sink_failure;
    AssistantEventSink guarded_sink =
        [&sink, &sink_failure](const AssistantStreamEvent& event) -> support::ExpectedVoid {
            auto emitted = emit(sink, event);
            if (!emitted) {
                sink_failure = emitted.error();
            }
            return emitted;
        };

    for (std::uint32_t attempt = 0;; ++attempt) {
        CCH_TRY(event_hook, attempt_hook());
        SseParser parser;
        bool started = false;
        bool saw_body = false;
        std::optional<support::Error> handler_failure;

        auto handle_chunk = [&](std::string_view bytes) -> support::ExpectedVoid {
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
                auto processed = event_hook(event, assistant, guarded_sink);
                if (!processed) {
                    handler_failure = processed.error();
                    return std::unexpected(processed.error());
                }
            }
            return {};
        };

        auto response = co_await transport.async_stream(request, handle_chunk);
        if (!response) {
            if (sink_failure) {
                co_return std::unexpected(*sink_failure);
            }
            if (handler_failure) {
                co_return complete_failure(
                    assistant, *handler_failure, guarded_sink);
            }
            if (response.error().code == support::ErrorCode::Cancelled ||
                options.stop_token.stop_requested()) {
                co_return complete_failure(
                    assistant,
                    support::make_error(
                        support::ErrorCode::Cancelled,
                        "Request was aborted"),
                    guarded_sink);
            }
            const auto failure = transport_failure(response.error());
            if (!saw_body) {
                CCH_TRY(retry, co_await retry_provider_failure(
                    failure,
                    attempt,
                    options));
                if (retry) {
                    continue;
                }
            }
            co_return complete_failure(
                assistant,
                normalize_transport_error(protocol_name, response.error()),
                guarded_sink);
        }

        if (response->head.status_code < 200 || response->head.status_code >= 300) {
            const auto failure = response_failure(*response);
            CCH_TRY(retry, co_await retry_provider_failure(
                failure,
                attempt,
                options));
            if (retry) {
                continue;
            }
            co_return complete_failure(
                assistant,
                stream_error(
                    std::format(
                        "{} request failed with HTTP {}",
                        protocol_name,
                        response->head.status_code),
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
            if (auto processed = event_hook(**final_event, assistant, guarded_sink); !processed) {
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
                support::make_error(
                    support::ErrorCode::Cancelled,
                    "Request was aborted"),
                guarded_sink);
        }
        if (finalize_hook) {
            if (auto finalized = finalize_hook(assistant); !finalized) {
                if (sink_failure) {
                    co_return std::unexpected(*sink_failure);
                }
                co_return complete_failure(
                    assistant, finalized.error(), guarded_sink);
            }
        }
        if (assistant.stop_reason == AssistantStopReason::Error) {
            co_return complete_failure(
                assistant,
                stream_error(assistant.error_message.value_or(
                    std::format("{} request failed", protocol_name))),
                guarded_sink);
        }
        CCH_TRY_VOID(emit(guarded_sink, AssistantDoneEvent{
            .reason = assistant.stop_reason,
            .message = assistant,
        }));
        co_return assistant;
    }
}

} // namespace cch::ai::providers
