#pragma once

#include <string>

namespace cch::ai {

enum class StopReason {
    Stop,
    ToolUse,
    Length,
    Error,
    Aborted,
    Unknown,
};

inline std::string to_string(StopReason reason) {
    switch (reason) {
    case StopReason::Stop:
        return "stop";
    case StopReason::ToolUse:
        return "tool_use";
    case StopReason::Length:
        return "length";
    case StopReason::Error:
        return "error";
    case StopReason::Aborted:
        return "aborted";
    case StopReason::Unknown:
        return "unknown";
    }
    return "unknown";
}

inline StopReason stop_reason_from_string(const std::string& reason) {
    if (reason == "stop") {
        return StopReason::Stop;
    }
    if (reason == "tool_use" || reason == "tool_calls") {
        return StopReason::ToolUse;
    }
    if (reason == "length") {
        return StopReason::Length;
    }
    if (reason == "error" || reason == "max_turns_exceeded" || reason == "provider_error") {
        return StopReason::Error;
    }
    if (reason == "aborted" || reason == "abort") {
        return StopReason::Aborted;
    }
    return StopReason::Unknown;
}

} // namespace cch::ai
