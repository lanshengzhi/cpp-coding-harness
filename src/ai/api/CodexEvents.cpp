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

namespace cch::ai::api {

[[nodiscard]] support::Expected<WsFrameAction> process_codex_json_event(JsonObject event,
        ResponsesEventProcessor& processor,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        CodexFailure* failure) {
    const auto type_found = event.find("type");
    const auto* type = type_found != event.end() ? type_found->second.get_if<std::string>() : nullptr;
    const bool response_failed = type && *type == "response.failed";
    const bool error_event = type && *type == "error";
    std::string serialized_event;
    if (error_event) {
        serialized_event = support::write_json(support::JsonValue{event}).value_or("{}");
    }

    auto processed = processor.process(std::move(event), assistant, sink);
    if (!processed) {
        return std::unexpected(processed.error());
    }
    if (processed->provider_error) {
        const auto& provider_error = *processed->provider_error;
        const auto inference_failure = InferenceFailure{
                .kind = provider_error.code ? providers::inference_failure_kind_from_provider_code(*provider_error.code)
                                            : InferenceFailureKind::InvalidRequest,
                .output_started = false,
                .suggested_backoff_ms = std::nullopt,
                .provider_code = provider_error.code,
        };
        if (failure) {
            failure->kind = CodexFailureKind::Api;
            failure->code = provider_error.code.value_or("");
            failure->message = provider_error.message.value_or("");
            failure->inference_failure = inference_failure;
        }
        std::string detail = provider_error.message.value_or("");
        if (response_failed && detail.empty()) {
            detail = "Codex response failed";
        }
        if (!response_failed && detail.empty()) {
            detail = provider_error.code.value_or("");
        }
        if (!response_failed && detail.empty()) {
            detail = serialized_event;
        }
        return std::unexpected(
                providers::make_stream_error(response_failed ? std::move(detail) : "Codex error: " + detail));
    }
    return processed->terminal ? WsFrameAction::Terminal : WsFrameAction::Continue;
}

[[nodiscard]] support::ExpectedVoid process_codex_sse_event(const providers::SseEvent& event,
        ResponsesEventProcessor& processor,
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
                .suggested_backoff_ms = std::nullopt,
                .provider_code = provider_code,
        };
        return std::unexpected(providers::make_stream_error(event.data));
    }
    auto parsed = support::read_json(event.data);
    if (!parsed) {
        if (event.event != "message" && !event.event.starts_with("response.")) {
            return {};
        }
        return std::unexpected(providers::make_stream_error("Invalid Codex SSE JSON: " + parsed.error().detail));
    }
    auto* event_object = parsed->get_if<JsonObject>();
    if (!event_object) {
        return std::unexpected(
                providers::make_stream_error("Malformed Codex SSE event", "event data must be a JSON object"));
    }
    CodexFailure failure;
    auto action = process_codex_json_event(std::move(*event_object), processor, assistant, sink, &failure);
    if (!action) {
        inference_failure = failure.inference_failure;
        return std::unexpected(action.error());
    }
    return {};
}

/// Adapter-owned terminal completion. The processor deliberately stops before
/// this policy: the adapter owns terminal sanitization, event commitment, and
/// transport-specific failure handling.
[[nodiscard]] support::Expected<AssistantMessage> complete_failure(AssistantMessage assistant,
        support::Error failure,
        AssistantEventSink& sink,
        std::optional<InferenceFailure> inference_failure,
        bool output_started) {
    for (auto& block : assistant.content) {
        auto* tool = std::get_if<ToolCallContent>(&block);
        if (tool && !tool->arguments) {
            finalize_tool_arguments(*tool);
        }
    }
    const auto aborted = failure.code == support::ErrorCode::Cancelled;
    assistant.stop_reason = aborted ? AssistantStopReason::Aborted : AssistantStopReason::Error;
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
        assistant.error_message = providers::bounded_provider_error_detail(std::move(diagnostic));
        failure = support::make_error(support::ErrorCode::Stream, *assistant.error_message);
    }
    if (!inference_failure) {
        inference_failure = InferenceFailure{
                .kind = providers::inference_failure_kind_from_transport(failure.code),
                .output_started = output_started,
        };
    } else {
        inference_failure->output_started = output_started;
    }
    auto emitted = providers::emit(sink,
            AssistantErrorEvent{
                    .reason = assistant.stop_reason,
                    .error = assistant,
                    .failure = std::move(failure),
                    .inference_failure = std::move(inference_failure),
            });
    if (!emitted) {
        return std::unexpected(emitted.error());
    }
    return assistant;
}

// ── WebSocket session cache ───────────────────────────────────────────────

void append_transport_diagnostic(AssistantMessage& assistant,
        const support::Error& error,
        std::string_view configured_transport,
        bool websocket_started,
        std::size_t request_bytes) {
    if (!assistant.diagnostics) {
        assistant.diagnostics.emplace();
    }
    assistant.diagnostics->push_back(DiagnosticEntry{
            .type = "provider_transport_failure",
            .timestamp = current_timestamp_ms(),
            .error =
                    DiagnosticErrorInfo{
                            .name = "Error",
                            .message = error.message,
                            .stack = std::nullopt,
                            .code = std::nullopt,
                    },
            .details =
                    support::JsonValue::object_t{
                            {"configuredTransport", std::string{configured_transport}},
                            {"fallbackTransport",
                                    websocket_started ? support::JsonValue{nullptr}
                                                      : support::JsonValue{std::string{"sse"}}},
                            {"eventsEmitted", websocket_started},
                            {"phase", websocket_started ? "after_message_stream_start" : "before_message_stream_start"},
                            {"requestBytes", static_cast<double>(request_bytes)},
                    },
    });
}

} // namespace cch::ai::api
