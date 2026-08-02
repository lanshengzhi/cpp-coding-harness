#pragma once

#include <cch/ai/Model.hpp>

namespace cch::agent::detail {

/// Mirrors pi Agent's internal DEFAULT_MODEL at parity baseline 83114817.
inline const ai::Model kDefaultModel{
    .id = "unknown",
    .name = "unknown",
    .api = "unknown",
    .provider = "unknown",
    .base_url = "",
    .reasoning = false,
    .thinking_level_map = std::nullopt,
    .input = {},
    .cost = {},
    .context_window = 0,
    .max_tokens = 0,
    .headers = std::nullopt,
    .compat = std::nullopt,
};

} // namespace cch::agent::detail
