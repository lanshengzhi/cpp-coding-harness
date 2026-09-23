#pragma once

#include <string_view>

namespace cch::coding_agent::tui {

/// pi v0.87.1 declares separate LEVEL_DESCRIPTIONS and THINKING_DESCRIPTIONS
/// tables in its two selectors; their level-to-description values are identical.
[[nodiscard]] inline std::string_view thinking_level_description(std::string_view level) {
    if (level == "off") return "No reasoning";
    if (level == "minimal") return "Very brief reasoning (~1k tokens)";
    if (level == "low") return "Light reasoning (~2k tokens)";
    if (level == "medium") return "Moderate reasoning (~8k tokens)";
    if (level == "high") return "Deep reasoning (~16k tokens)";
    if (level == "xhigh") return "Extra-high reasoning (~32k tokens)";
    if (level == "max") return "Maximum reasoning";
    return {};
}

} // namespace cch::coding_agent::tui
