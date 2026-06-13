#pragma once

#include <cstdint>

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

} // namespace cch::ai
