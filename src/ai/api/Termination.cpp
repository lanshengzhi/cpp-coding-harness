#include "Termination.hpp"

#include <string>

namespace cch::ai::api {

util::Expected<TerminationResult> map_responses_termination(
    std::string_view terminal,
    bool has_tool_call) {
    if (terminal == "completed" || terminal == "done") {
        return TerminationResult{
            .reason = has_tool_call ? AssistantStopReason::ToolUse : AssistantStopReason::Stop,
        };
    }
    if (terminal == "incomplete") {
        return TerminationResult{.reason = AssistantStopReason::Length};
    }
    if (terminal == "failed" || terminal == "cancelled") {
        return TerminationResult{
            .reason = AssistantStopReason::Error,
            .error_message = "Responses request failed",
        };
    }
    if (terminal.empty() || terminal == "missing") {
        return TerminationResult{
            .reason = AssistantStopReason::Error,
            .error_message = "Responses stream ended without a terminal event",
        };
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Stream,
        "Unhandled Responses terminal status: " + std::string{terminal}));
}

util::Expected<TerminationResult> map_anthropic_termination(
    std::string_view stop_reason,
    std::optional<std::string_view> refusal_explanation) {
    if (stop_reason == "end_turn" || stop_reason == "pause_turn" ||
        stop_reason == "stop_sequence") {
        return TerminationResult{.reason = AssistantStopReason::Stop};
    }
    if (stop_reason == "max_tokens") {
        return TerminationResult{.reason = AssistantStopReason::Length};
    }
    if (stop_reason == "tool_use") {
        return TerminationResult{.reason = AssistantStopReason::ToolUse};
    }
    if (stop_reason == "refusal") {
        return TerminationResult{
            .reason = AssistantStopReason::Error,
            .error_message = refusal_explanation
                ? std::string{*refusal_explanation}
                : "The model refused to complete the request",
        };
    }
    if (stop_reason == "sensitive") {
        return TerminationResult{
            .reason = AssistantStopReason::Error,
            .error_message = "Provider stopped with: sensitive",
        };
    }
    if (stop_reason.empty() || stop_reason == "missing") {
        return TerminationResult{
            .reason = AssistantStopReason::Error,
            .error_message = "Anthropic stream ended without message_stop",
        };
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Stream,
        "Unhandled Anthropic stop reason: " + std::string{stop_reason}));
}

} // namespace cch::ai::api
