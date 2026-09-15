#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

/// One row of the pi `stopReason` wire vocabulary. The table below is the one
/// source for both directions, so the serializer and any parser reading this
/// vocabulary cannot drift apart (#665).
struct StopReasonWireName {
    AssistantStopReason reason{};
    std::string_view wire_name{};
};

inline constexpr std::array<StopReasonWireName, 6> kStopReasonNames{{
        {AssistantStopReason::Pending, "pending"},
        {AssistantStopReason::Stop, "stop"},
        {AssistantStopReason::Length, "length"},
        {AssistantStopReason::ToolUse, "toolUse"},
        {AssistantStopReason::Error, "error"},
        {AssistantStopReason::Aborted, "aborted"},
}};

[[nodiscard]] inline std::string stop_reason_to_string(AssistantStopReason reason) {
    for (const auto& entry : kStopReasonNames) {
        if (entry.reason == reason) {
            return std::string{entry.wire_name};
        }
    }
    return "error";
}

/// The inverse of `stop_reason_to_string`; `std::nullopt` for a wire value the
/// vocabulary does not carry.
[[nodiscard]] inline std::optional<AssistantStopReason> stop_reason_from_string(std::string_view wire_name) {
    for (const auto& entry : kStopReasonNames) {
        if (entry.wire_name == wire_name) {
            return entry.reason;
        }
    }
    return std::nullopt;
}

} // namespace cch::ai
