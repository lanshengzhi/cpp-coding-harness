#include "Termination.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

namespace cch::ai::api {
namespace {

struct TerminationMapping {
    std::string_view status;
    AssistantStopReason reason;
    std::string_view error;
    // `completed` doubles as the tool-call terminal: upgrade Stop to ToolUse
    // when the assistant already holds a tool call.
    bool upgrade_to_tool_use{false};
    // `refusal` carries the provider's explanation when one is present.
    bool prefer_explanation{false};
};

constexpr TerminationMapping kResponsesMappings[] = {
        {"completed", AssistantStopReason::Stop, {}, true, false},
        {"done", AssistantStopReason::Stop, {}, true, false},
        {"incomplete", AssistantStopReason::Length, {}, false, false},
        {"failed", AssistantStopReason::Error, "Responses request failed", false, false},
        {"cancelled", AssistantStopReason::Error, "Responses request failed", false, false},
        {"missing", AssistantStopReason::Error, "Responses stream ended without a terminal event", false, false},
};

constexpr TerminationMapping kAnthropicMappings[] = {
        {"end_turn", AssistantStopReason::Stop, {}, false, false},
        {"pause_turn", AssistantStopReason::Stop, {}, false, false},
        {"stop_sequence", AssistantStopReason::Stop, {}, false, false},
        {"max_tokens", AssistantStopReason::Length, {}, false, false},
        {"tool_use", AssistantStopReason::ToolUse, {}, false, false},
        {"refusal", AssistantStopReason::Error, "The model refused to complete the request", false, true},
        {"sensitive", AssistantStopReason::Error, "Provider stopped with: sensitive", false, false},
        {"missing", AssistantStopReason::Error, "Anthropic stream ended without message_stop", false, false},
};

[[nodiscard]] const TerminationMapping* find_mapping(
        const TerminationMapping* begin, const TerminationMapping* end, std::string_view status) {
    const auto found = std::ranges::find(begin, end, status, &TerminationMapping::status);
    return found == end ? nullptr : found;
}

} // namespace

support::Expected<TerminationResult> map_responses_termination(
    std::string_view terminal,
    bool has_tool_call) {
    const std::string_view key = terminal.empty() ? "missing" : terminal;
    const auto* mapping = find_mapping(std::begin(kResponsesMappings), std::end(kResponsesMappings), key);
    if (!mapping) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Stream, "Unhandled Responses terminal status: " + std::string{terminal}));
    }
    TerminationResult result{.reason = mapping->reason};
    if (mapping->upgrade_to_tool_use && has_tool_call) {
        result.reason = AssistantStopReason::ToolUse;
    }
    if (mapping->reason == AssistantStopReason::Error) {
        result.error_message = std::string{mapping->error};
    }
    return result;
}

support::Expected<TerminationResult> map_anthropic_termination(
    std::string_view stop_reason,
    std::optional<std::string_view> refusal_explanation) {
    const std::string_view key = stop_reason.empty() ? "missing" : stop_reason;
    const auto* mapping = find_mapping(std::begin(kAnthropicMappings), std::end(kAnthropicMappings), key);
    if (!mapping) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Stream, "Unhandled Anthropic stop reason: " + std::string{stop_reason}));
    }
    TerminationResult result{.reason = mapping->reason};
    if (mapping->reason == AssistantStopReason::Error) {
        result.error_message = mapping->prefer_explanation && refusal_explanation ? std::string{*refusal_explanation}
                                                                                  : std::string{mapping->error};
    }
    return result;
}

} // namespace cch::ai::api
