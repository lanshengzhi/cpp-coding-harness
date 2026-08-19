#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamTransport.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <string_view>

namespace cch::ai::providers {

/// Invoked per decoded SSE event during one HTTP streaming attempt.
using SseEventHook = std::move_only_function<
    support::ExpectedVoid(
        const SseEvent& event,
        AssistantMessage& assistant,
        AssistantEventSink& sink)>;

/// Factory producing a fresh event hook (and resetting attempt slot state) per attempt.
using SseAttemptHook = std::move_only_function<
    support::Expected<SseEventHook>()>;

/// Optional post-stream finalization hook (e.g. verifying terminal event received).
using SseFinalizeHook = std::move_only_function<
    support::ExpectedVoid(AssistantMessage& assistant)>;

/// Options configuring one execution of the SSE streaming pipeline.
struct SseStreamExecutionOptions {
    std::string_view protocol_name{"Provider"};
    /// Borrowed request inputs must outlive the coroutine suspension life cycle (§7.5).
    const StreamRequest& request;
    /// Transport executing raw HTTP stream requests; must outlive coroutine (§7.5).
    StreamTransport& transport;
    /// Provider options supplying retry limits and cancellation tokens; must outlive coroutine (§7.5).
    const ProviderStreamOptions& options;
    AssistantMessage initial_assistant;
    AssistantEventSink sink;
    SseAttemptHook attempt_hook;
    SseFinalizeHook finalize_hook{nullptr};
};

/// Unified SSE stream execution engine: manages HTTP streaming, chunk ingestion,
/// SSE parsing, first-byte AssistantStartEvent, exponential backoff retries with
/// stop_token cancellation, safe diagnostic bounding, terminal error emission,
/// and caller sink error isolation.
[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>>
execute_sse_stream(SseStreamExecutionOptions execution_options);

} // namespace cch::ai::providers
