#pragma once

#include <cch/ai/Model.hpp>

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace cch::ai::detail {

inline constexpr std::array<std::pair<ModelThinkingLevel, std::string_view>, 7>
    kModelThinkingLevels{{
        {ModelThinkingLevel::Off, "off"},
        {ModelThinkingLevel::Minimal, "minimal"},
        {ModelThinkingLevel::Low, "low"},
        {ModelThinkingLevel::Medium, "medium"},
        {ModelThinkingLevel::High, "high"},
        {ModelThinkingLevel::XHigh, "xhigh"},
        {ModelThinkingLevel::Max, "max"},
    }};

[[nodiscard]] inline std::optional<std::string_view> model_thinking_level_name(
    ModelThinkingLevel level) {
    for (const auto& [known_level, name] : kModelThinkingLevels) {
        if (known_level == level) {
            return name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<ModelThinkingLevel> parse_model_thinking_level(
    std::string_view name) {
    for (const auto& [level, known_name] : kModelThinkingLevels) {
        if (known_name == name) {
            return level;
        }
    }
    return std::nullopt;
}

} // namespace cch::ai::detail
