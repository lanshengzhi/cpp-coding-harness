#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cch::ai {

struct Model;

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
    std::optional<std::int64_t> cache_write_1h;
    std::optional<std::int64_t> reasoning;
    std::int64_t total_tokens{};
    UsageCost cost{};
};

[[nodiscard]] UsageCost calculate_cost(const Model& model, const Usage& usage);

enum class AssistantStopReason {
    Pending,
    Stop,
    ToolUse,
    Length,
    Error,
    Aborted,
};

[[nodiscard]] inline std::string stop_reason_to_string(AssistantStopReason reason) {
    switch (reason) {
    case AssistantStopReason::Pending:
        return "pending";
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
    }
    return "error";
}

} // namespace cch::ai
