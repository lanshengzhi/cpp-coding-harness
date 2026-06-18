#pragma once

#include <cstdint>
#include <string>

namespace cch::ai {

struct UsageCost {
    double input{};
    double output{};
    double cache_read{};
    double cache_write{};
    double total{};
};

struct Usage {
    std::int64_t input{};
    std::int64_t output{};
    std::int64_t cache_read{};
    std::int64_t cache_write{};
    std::int64_t total_tokens{};
    UsageCost cost{};
};

enum class AssistantStopReason {
    Stop,
    ToolUse,
    Length,
    Error,
    Aborted,
    Unknown,
};

[[nodiscard]] inline std::string stop_reason_to_string(AssistantStopReason reason) {
    switch (reason) {
    case AssistantStopReason::Stop:
        return "stop";
    case AssistantStopReason::Length:
        return "length";
    case AssistantStopReason::ToolUse:
        return "toolUse";
    case AssistantStopReason::Error:
        return "error";
    case AssistantStopReason::Aborted:
        return "aborted";
    case AssistantStopReason::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace cch::ai
