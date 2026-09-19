#include "ResponsesEventProcessor.hpp"

#include "ai/JsonAccess.hpp"
#include "ai/Timestamps.hpp"
#include "ai/api/PartialJson.hpp"
#include "ai/api/Termination.hpp"
#include "ai/api/UsageNormalization.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include "ResponsesSlots.hpp"

namespace cch::ai::api {
namespace {

[[nodiscard]] ResponsesProviderError provider_error(const JsonObject& event) {
    const auto* nested = json_object_member(event, "error");
    auto code = json_string_member(event, "code");
    if (!code && nested) {
        code = json_string_member(*nested, "code");
    }
    auto message = json_string_member(event, "message");
    if (!message && nested) {
        message = json_string_member(*nested, "message");
    }
    const auto now = current_timestamp_ms();
    return ResponsesProviderError{
            .code = code ? std::optional<std::string>{std::string{*code}} : std::nullopt,
            .message = message ? std::optional<std::string>{std::string{*message}} : std::nullopt,
            .suggested_backoff_ms = providers::provider_backoff_hint_ms(event, now),
    };
}

void apply_deepseek_usage(const Model& model, const JsonObject& response, AssistantMessage& assistant) {
    const auto* usage = json_object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = json_object_member(*usage, "input_tokens_details");
    const auto* output_details = json_object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_deepseek_usage(model,
            json_integer_member(*usage, "input_tokens").value_or(0),
            json_integer_member(*usage, "output_tokens").value_or(0),
            input_details ? json_integer_member(*input_details, "cached_tokens").value_or(0) : 0,
            output_details ? json_integer_member(*output_details, "reasoning_tokens") : std::nullopt);
    if (const auto total = json_integer_member(*usage, "total_tokens")) {
        assistant.usage.total_tokens = *total;
    }
}

void apply_codex_usage(const Model& model, const JsonObject& response, AssistantMessage& assistant) {
    const auto* usage = json_object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = json_object_member(*usage, "input_tokens_details");
    const auto* output_details = json_object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_responses_usage(model,
            ResponsesUsageFields{
                    .input_tokens = json_integer_member(*usage, "input_tokens").value_or(0),
                    .output_tokens = json_integer_member(*usage, "output_tokens").value_or(0),
                    .cached_tokens =
                            input_details ? json_integer_member(*input_details, "cached_tokens").value_or(0) : 0,
                    .cache_write_tokens =
                            input_details ? json_integer_member(*input_details, "cache_write_tokens").value_or(0) : 0,
                    .reasoning_tokens =
                            output_details ? json_integer_member(*output_details, "reasoning_tokens") : std::nullopt,
                    .total_tokens = json_integer_member(*usage, "total_tokens").value_or(0),
            });
}

[[nodiscard]] support::Expected<ResponsesProcessOutcome> finalize_response(const Model& model,
        ResponsesDialect dialect,
        const JsonObject& event,
        std::string_view event_type,
        AssistantMessage& assistant,
        bool& saw_terminal) {
    // pi's `processResponsesStream` records that a terminal response event
    // arrived before the adapter's final integrity check.
    saw_terminal = true;
    const auto* response = json_object_member(event, "response");
    if (!response) {
        if (dialect == ResponsesDialect::Codex) {
            assistant.stop_reason = AssistantStopReason::Stop;
            return ResponsesProcessOutcome{.terminal = true};
        }
        return std::unexpected(support::make_error(
                support::ErrorCode::Stream, "OpenAI Responses terminal event omitted response data"));
    }
    if (const auto id = json_string_member(*response, "id"); id && !id->empty()) {
        assistant.response_id = std::string{*id};
    }
    if (const auto model_id = json_string_member(*response, "model"); model_id && *model_id != model.id) {
        assistant.response_model = std::string{*model_id};
    }
    if (dialect == ResponsesDialect::DeepSeek) {
        // pi's `finalizeResponse` records the raw wire status for every
        // terminal Responses event, including failures.
        if (const auto status = json_string_member(*response, "status"); status && !status->empty()) {
            assistant.raw_stop_reason = *status;
        }
        apply_deepseek_usage(model, *response, assistant);
        if (event_type == "response.failed") {
            return ResponsesProcessOutcome{
                    .terminal = true,
                    .provider_error = provider_error(*response),
            };
        }
    } else {
        apply_codex_usage(model, *response, assistant);
    }

    const auto status = json_string_member(*response, "status").value_or(event_type == "response.done" ? "done" : "");
    std::string_view normalized = status;
    if (dialect == ResponsesDialect::Codex) {
        normalized = "done";
        if (status == "completed" || status == "incomplete" || status == "failed" || status == "cancelled" ||
                status == "queued" || status == "in_progress") {
            if (status == "completed" || status == "incomplete" || status == "failed" || status == "cancelled") {
                normalized = status;
            }
            // pi's Codex mapper records only recognized statuses as raw stop
            // reasons; queued/in_progress normalize to stop through `done`.
            assistant.raw_stop_reason = status;
        }
    }
    auto termination = map_responses_termination(normalized,
            std::ranges::any_of(assistant.content,
                    [](const AssistantContent& block) { return std::holds_alternative<ToolCallContent>(block); }));
    if (!termination) {
        return std::unexpected(termination.error());
    }
    assistant.stop_reason = termination->reason;
    assistant.error_message = termination->error_message;
    return ResponsesProcessOutcome{.terminal = true};
}

[[nodiscard]] support::Expected<ResponsesProcessOutcome> process_event(const Model& model,
        ResponsesDialect dialect,
        ResponsesDelivery delivery,
        JsonObject event,
        std::map<std::size_t, Slot>& slots,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        bool& saw_terminal) {
    static constexpr std::string_view kDeltaEvents[] = {
            "response.reasoning_summary_text.delta",
            "response.reasoning_text.delta",
            "response.output_text.delta",
            "response.refusal.delta",
            "response.function_call_arguments.delta",
    };
    static constexpr std::string_view kTerminalEvents[] = {
            "response.completed",
            "response.done",
            "response.incomplete",
    };
    static constexpr std::string_view kFailureEvents[] = {
            "response.failed",
            "error",
    };
    const auto type = json_string_member(event, "type");
    if (!type) {
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.created") {
        if (const auto* response = json_object_member(event, "response")) {
            if (const auto id = json_string_member(*response, "id"); id && !id->empty()) {
                assistant.response_id = std::string{*id};
            }
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.output_item.added") {
        const auto index = output_index(event);
        const auto* item = json_object_member(event, "item");
        if (index && item) {
            if (auto created = create_slot(*index, *item, slots, assistant, sink); !created) {
                return std::unexpected(created.error());
            }
        }
        return ResponsesProcessOutcome{};
    }
    if (std::ranges::contains(kDeltaEvents, *type)) {
        if (auto processed = append_delta(event, *type, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.reasoning_summary_part.done") {
        if (auto processed = append_reasoning_separator(event, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.function_call_arguments.done") {
        if (auto processed = finish_argument_stream(event, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (*type == "response.output_item.done") {
        if (auto processed = finish_output_item(event, slots, assistant, sink); !processed) {
            return std::unexpected(processed.error());
        }
        return ResponsesProcessOutcome{};
    }
    if (std::ranges::contains(kTerminalEvents, *type)) {
        return finalize_response(model, dialect, event, *type, assistant, saw_terminal);
    }
    if (std::ranges::contains(kFailureEvents, *type)) {
        if (dialect == ResponsesDialect::DeepSeek && *type == "response.failed") {
            return finalize_response(model, dialect, event, *type, assistant, saw_terminal);
        }
        auto failure = provider_error(event);
        if (*type == "response.failed" && delivery == ResponsesDelivery::WebSocket) {
            // Codex WebSocket failures carry retryable codes under response.error;
            // SSE retains its historical top-level error extraction.
            if (const auto* response = json_object_member(event, "response")) {
                auto response_failure = provider_error(*response);
                if (!failure.code) {
                    failure.code = std::move(response_failure.code);
                }
                if (!failure.message) {
                    failure.message = std::move(response_failure.message);
                }
                if (!failure.suggested_backoff_ms) {
                    failure.suggested_backoff_ms = std::move(response_failure.suggested_backoff_ms);
                }
            }
        }
        return ResponsesProcessOutcome{
                .terminal = false,
                .provider_error = std::move(failure),
        };
    }
    return ResponsesProcessOutcome{};
}

} // namespace

struct ResponsesEventProcessor::Impl {
    Impl(ResponsesDialect configured_dialect, ResponsesDelivery configured_delivery, Model configured_model)
        : dialect(configured_dialect), delivery(configured_delivery), model(std::move(configured_model)) {}

    ResponsesDialect dialect;
    ResponsesDelivery delivery;
    Model model;
    std::map<std::size_t, Slot> slots;
    bool saw_terminal{false};
};

ResponsesEventProcessor::ResponsesEventProcessor(ResponsesDialect dialect, ResponsesDelivery delivery, Model model)
    : impl_(std::make_unique<Impl>(dialect, delivery, std::move(model))) {}

ResponsesEventProcessor::ResponsesEventProcessor(ResponsesEventProcessor&&) noexcept = default;
ResponsesEventProcessor& ResponsesEventProcessor::operator=(ResponsesEventProcessor&&) noexcept = default;
ResponsesEventProcessor::~ResponsesEventProcessor() = default;

support::Expected<ResponsesProcessOutcome> ResponsesEventProcessor::process(
        JsonObject event, AssistantMessage& assistant, AssistantEventSink& sink) {
    return process_event(impl_->model,
            impl_->dialect,
            impl_->delivery,
            std::move(event),
            impl_->slots,
            assistant,
            sink,
            impl_->saw_terminal);
}

support::ExpectedVoid ResponsesEventProcessor::finish([[maybe_unused]] AssistantMessage&) {
    if (!impl_->saw_terminal) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Stream, "OpenAI Responses stream ended before a terminal response event"));
    }
    return {};
}

} // namespace cch::ai::api