#pragma once

#include "CodexShared.hpp"

#include "ai/api/ResponsesEventProcessor.hpp"

#include <cch/ai/StreamEvent.hpp>
#include <cch/ai/InferenceFailure.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace cch::ai::api {

enum class WsFrameAction { Continue, Terminal };

/// Codex event processing shared by the WebSocket and SSE paths: one JSON
/// frame → stream events, one SSE event, and the terminal-failure composer.

[[nodiscard]] support::Expected<WsFrameAction> process_codex_json_event(support::JsonValue::object_t event,
        ResponsesEventProcessor& processor,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        CodexFailure* failure);

[[nodiscard]] support::ExpectedVoid process_codex_sse_event(const providers::SseEvent& event,
        ResponsesEventProcessor& processor,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        std::optional<InferenceFailure>& inference_failure);

[[nodiscard]] support::Expected<AssistantMessage> complete_failure(AssistantMessage assistant,
        support::Error failure,
        AssistantEventSink& sink,
        std::optional<InferenceFailure> inference_failure = std::nullopt,
        bool output_started = false);

void append_transport_diagnostic(AssistantMessage& assistant,
        const support::Error& error,
        std::string_view configured_transport,
        bool websocket_started,
        std::size_t request_bytes);

/// Terminal path for a completed WebSocket attempt: cancellation, provider
/// failure, or the finished assistant message.
[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> finish_ws_completed(
        AssistantMessage assistant, bool started, const std::stop_token& stop_token, AssistantEventSink& sink);

} // namespace cch::ai::api
