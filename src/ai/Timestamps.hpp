#pragma once

#include <cch/ai/Message.hpp>

#include <chrono>

namespace cch::ai {

/// Wall-clock now in milliseconds, the TimestampMs domain type.
[[nodiscard]] inline TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

} // namespace cch::ai
