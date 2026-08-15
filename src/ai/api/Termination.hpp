#pragma once

#include <cch/ai/Usage.hpp>
#include <cch/support/Error.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::api {

struct TerminationResult {
    AssistantStopReason reason{AssistantStopReason::Stop};
    std::optional<std::string> error_message{std::nullopt};
};

[[nodiscard]] support::Expected<TerminationResult> map_responses_termination(
    std::string_view terminal,
    bool has_tool_call);
[[nodiscard]] support::Expected<TerminationResult> map_anthropic_termination(
    std::string_view stop_reason,
    std::optional<std::string_view> refusal_explanation = std::nullopt);

} // namespace cch::ai::api
