#pragma once

#include <cch/ai/InferenceFailure.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/ai/Timestamps.hpp>
#include "ai/ModelsInternal.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmit.hpp"
#include <cch/support/BoundedText.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cch::ai::providers {

inline void ensure_tool_arguments_allocated(AssistantMessage& assistant) {
    for (auto& content : assistant.content) {
        auto* tool = std::get_if<ToolCallContent>(&content);
        if (tool && !tool->arguments) {
            tool->arguments = support::JsonValue{support::JsonValue::object_t{}};
            tool->arguments_valid = true;
            tool->argument_error = std::nullopt;
        }
    }
}

[[nodiscard]] constexpr bool is_models_domain_error(support::ErrorCode code) noexcept {
    return code == support::ErrorCode::ModelSource || code == support::ErrorCode::ModelValidation ||
           code == support::ErrorCode::Provider || code == support::ErrorCode::Stream ||
           code == support::ErrorCode::Auth || code == support::ErrorCode::OAuth;
}

[[nodiscard]] inline std::string public_error_diagnostic(const support::Error& error) {
    std::string diagnostic = error.message;
    if (!error.detail.empty() && diagnostic.find(error.detail) == std::string::npos) {
        if (!diagnostic.empty()) {
            diagnostic += ": ";
        }
        diagnostic += error.detail;
    }
    return support::bounded_redacted_text(std::move(diagnostic), kMaxPublicErrorBytes, "...");
}

/// Unified stream lifecycle and terminal error emission coordinator.
/// Enforces single-terminal invariants, orthogonal sink error isolation,
/// non-reversible output_started tracking, and error sanitization.
class StreamEmitter final {
public:
    explicit StreamEmitter(AssistantMessage initial_assistant, AssistantEventSink sink, std::string request_key = {})
        : assistant_(std::move(initial_assistant)), sink_(std::move(sink)), request_key_(std::move(request_key)) {}

    StreamEmitter(StreamEmitter&&) noexcept = default;
    StreamEmitter& operator=(StreamEmitter&&) noexcept = default;
    ~StreamEmitter() = default;
    StreamEmitter(const StreamEmitter&) = delete;
    StreamEmitter& operator=(const StreamEmitter&) = delete;

    [[nodiscard]] support::ExpectedVoid emit_start() {
        if (started_) {
            return {};
        }
        started_ = true;
        if (sink_failure_) {
            return std::unexpected(*sink_failure_);
        }
        if (!sink_) {
            return {};
        }
        if (auto emitted = sink_(AssistantStartEvent{.partial = assistant_}); !emitted) {
            sink_failure_ = emitted.error();
            return std::unexpected(*sink_failure_);
        }
        return {};
    }

    [[nodiscard]] support::ExpectedVoid emit_event(const AssistantStreamEvent& event) {
        if (sink_failure_) {
            return std::unexpected(*sink_failure_);
        }

        if (const auto* done = std::get_if<AssistantDoneEvent>(&event)) {
            auto finalized = finalize_success(*done);
            if (!finalized) {
                return std::unexpected(finalized.error());
            }
            return {};
        }

        if (const auto* error = std::get_if<AssistantErrorEvent>(&event)) {
            auto finalized = finalize_error_event(*error);
            if (!finalized) {
                return std::unexpected(finalized.error());
            }
            return {};
        }

        if (terminated_) {
            return {};
        }
        if (const auto* start = std::get_if<AssistantStartEvent>(&event)) {
            assistant_ = start->partial;
        } else {
            std::visit(
                    [this](const auto& concrete) {
                        if constexpr (requires { concrete.partial; }) {
                            assistant_ = concrete.partial;
                        }
                    },
                    event);
        }
        if (std::holds_alternative<TextDeltaEvent>(event) || std::holds_alternative<ThinkingDeltaEvent>(event) ||
                std::holds_alternative<ToolCallStartEvent>(event) ||
                std::holds_alternative<ToolCallDeltaEvent>(event) || std::holds_alternative<ToolCallEndEvent>(event)) {
            output_started_ = true;
        }
        if (!sink_) {
            return {};
        }
        if (auto emitted = sink_(event); !emitted) {
            sink_failure_ = emitted.error();
            return std::unexpected(*sink_failure_);
        }
        return {};
    }

    /// Returns a non-owning sink adapter forwarding events to this emitter.
    /// The returned sink must not outlive *this (§6.2, §7.5).
    [[nodiscard]] AssistantEventSink as_sink() noexcept {
        return [this](const AssistantStreamEvent& event) -> support::ExpectedVoid { return emit_event(event); };
    }

    [[nodiscard]] support::Expected<AssistantMessage> finalize_success(const AssistantDoneEvent& terminal) {
        if (sink_failure_) {
            return std::unexpected(*sink_failure_);
        }
        if (terminated_) {
            return assistant_;
        }
        if (terminal.reason == AssistantStopReason::Error || terminal.reason == AssistantStopReason::Aborted ||
                terminal.message.stop_reason == AssistantStopReason::Error ||
                terminal.message.stop_reason == AssistantStopReason::Aborted) {
            return finalize_error_event(error_event_from_done(terminal));
        }
        terminated_ = true;
        assistant_ = terminal.message;
        ensure_tool_arguments_allocated(assistant_);
        assistant_.stop_reason = terminal.reason;
        if (assistant_.error_message) {
            assistant_.error_message = safe_diagnostic_text(std::move(*assistant_.error_message), request_key_);
        }
        if (sink_) {
            if (auto emitted = sink_(AssistantDoneEvent{
                        .reason = terminal.reason,
                        .message = assistant_,
                });
                    !emitted) {
                sink_failure_ = emitted.error();
                return std::unexpected(*sink_failure_);
            }
        }
        return assistant_;
    }

    [[nodiscard]] static AssistantErrorEvent error_event_from_done(const AssistantDoneEvent& terminal) {
        auto failure = support::make_error(support::ErrorCode::Stream, std::string{});
        return AssistantErrorEvent{
                .reason = terminal.reason,
                .error = terminal.message,
                .failure = std::move(failure),
        };
    }

    [[nodiscard]] support::Expected<AssistantMessage> finalize_error_event(AssistantErrorEvent terminal) {
        if (sink_failure_) {
            return std::unexpected(*sink_failure_);
        }
        if (terminated_) {
            return assistant_;
        }
        terminated_ = true;
        // The terminal message is authoritative; what it leaves unset is
        // inherited from the message observed while streaming, so a sparse
        // provider terminal keeps the partial output and stream identity
        // already seen (#1042).
        if (terminal.error.content.empty()) {
            terminal.error.content = std::move(assistant_.content);
        }
        if (terminal.error.api.empty()) {
            terminal.error.api = std::move(assistant_.api);
        }
        if (terminal.error.provider.empty()) {
            terminal.error.provider = std::move(assistant_.provider);
        }
        if (terminal.error.model.empty()) {
            terminal.error.model = std::move(assistant_.model);
        }
        if (terminal.error.timestamp == 0) {
            terminal.error.timestamp = assistant_.timestamp;
        }
        assistant_ = std::move(terminal.error);
        ensure_tool_arguments_allocated(assistant_);

        auto inference_failure = std::move(terminal.inference_failure);
        const bool aborted = terminal.reason == AssistantStopReason::Aborted ||
                             assistant_.stop_reason == AssistantStopReason::Aborted ||
                             (terminal.failure && terminal.failure->code == support::ErrorCode::Cancelled) ||
                             (inference_failure && inference_failure->kind == InferenceFailureKind::Cancelled);
        auto failure =
                terminal.failure
                        ? safe_error(std::move(*terminal.failure), request_key_)
                        : support::make_error(aborted ? support::ErrorCode::Cancelled : support::ErrorCode::Stream,
                                  assistant_.error_message.value_or(
                                          aborted ? std::string{kDefaultAbortDiagnostic} : "Provider stream failed"));
        if (!inference_failure) {
            inference_failure = InferenceFailure{
                    .kind = inference_failure_kind_from_transport(failure.code),
                    .output_started = output_started_,
            };
        }

        if (!assistant_.error_message || assistant_.error_message->empty()) {
            const auto diagnostic = !failure.message.empty() || !failure.detail.empty()
                                            ? public_error_diagnostic(failure)
                                            : std::string{};
            if (!diagnostic.empty()) {
                assistant_.error_message = diagnostic;
            } else if (aborted) {
                assistant_.error_message = std::string{kDefaultAbortDiagnostic};
            } else {
                assistant_.error_message = "Provider stream failed";
            }
        }
        if (assistant_.error_message && !assistant_.error_message->empty()) {
            assistant_.error_message = safe_diagnostic_text(std::move(*assistant_.error_message), request_key_);
            failure.message = *assistant_.error_message;
        } else {
            failure.message = public_error_diagnostic(failure);
        }
        if (aborted) {
            assistant_.stop_reason = AssistantStopReason::Aborted;
            failure.code = support::ErrorCode::Cancelled;
        } else {
            assistant_.stop_reason = AssistantStopReason::Error;
            failure.code = is_models_domain_error(failure.code)
                                   ? failure.code
                                   : (inference_failure && requires_reauthentication(*inference_failure)
                                                     ? support::ErrorCode::Auth
                                                     : support::ErrorCode::Stream);
        }
        inference_failure->output_started = output_started_;
        if (aborted) {
            inference_failure->kind = InferenceFailureKind::Cancelled;
        }

        if (sink_) {
            if (auto emitted = sink_(AssistantErrorEvent{
                        .reason = assistant_.stop_reason,
                        .error = assistant_,
                        .failure = std::move(failure),
                        .inference_failure = std::move(inference_failure),
                });
                    !emitted) {
                sink_failure_ = emitted.error();
                return std::unexpected(*sink_failure_);
            }
        }
        if (sink_failure_) {
            return std::unexpected(*sink_failure_);
        }
        return assistant_;
    }

    [[nodiscard]] support::Expected<AssistantMessage> finalize_failure(support::Error failure,
            std::optional<InferenceFailure> inference_failure = std::nullopt,
            AssistantStopReason reason = AssistantStopReason::Error) {
        auto terminal_message = assistant_;
        return finalize_error_event(AssistantErrorEvent{
                .reason = reason,
                .error = std::move(terminal_message),
                .failure = std::move(failure),
                .inference_failure = std::move(inference_failure),
        });
    }

    [[nodiscard]] support::Expected<AssistantMessage> finalize_success(
            AssistantStopReason reason = AssistantStopReason::Stop) {
        auto message = assistant_;
        message.stop_reason = reason;
        message.timestamp = current_timestamp_ms();
        return finalize_success(AssistantDoneEvent{.reason = reason, .message = std::move(message)});
    }

    [[nodiscard]] support::Expected<AssistantMessage> finalize_cancelled() {
        auto message = assistant_;
        message.stop_reason = AssistantStopReason::Aborted;
        return finalize_error_event(AssistantErrorEvent{
                .reason = AssistantStopReason::Aborted,
                .error = std::move(message),
                .failure = support::make_error(support::ErrorCode::Cancelled, std::string{kDefaultAbortDiagnostic}),
                .inference_failure =
                        InferenceFailure{
                                .kind = InferenceFailureKind::Cancelled,
                                .output_started = output_started_,
                                .suggested_backoff_ms = std::nullopt,
                                .provider_code = std::nullopt,
                        },
        });
    }

    [[nodiscard]] bool output_started() const noexcept { return output_started_; }
    [[nodiscard]] bool is_terminated() const noexcept { return terminated_; }
    [[nodiscard]] bool sink_broken() const noexcept { return sink_failure_.has_value(); }
    [[nodiscard]] const std::optional<support::Error>& sink_failure() const noexcept { return sink_failure_; }
    [[nodiscard]] AssistantMessage& assistant() noexcept { return assistant_; }
    [[nodiscard]] const AssistantMessage& assistant() const noexcept { return assistant_; }

private:
    AssistantMessage assistant_;
    AssistantEventSink sink_;
    std::string request_key_{};
    bool started_{false};
    bool output_started_{false};
    bool terminated_{false};
    std::optional<support::Error> sink_failure_{std::nullopt};
};

} // namespace cch::ai::providers
