#pragma once

#include <cch/ai/Model.hpp>

#include <optional>
#include <string>
#include <utility>

namespace cch::tests {

[[nodiscard]] inline ai::Model make_model(
    std::string id,
    std::string provider = "fake",
    std::string api = "scripted-fake") {
    return ai::Model{
        .id = id,
        .name = std::move(id),
        .api = std::move(api),
        .provider = std::move(provider),
        .base_url = "https://example.invalid",
        .reasoning = false,
        .thinking_level_map = std::nullopt,
        .input = {ai::ModelInput::Text},
        .cost = {},
        .context_window = 100000,
        .max_tokens = 4096,
        .headers = std::nullopt,
        .compat = std::nullopt,
    };
}

/// Reasoning model whose map explicitly maps every seven-level name, so all
/// levels are supported and nothing is clamped away. Used where a test needs
/// the pi default "medium" (or another requested level) to pass through
/// creation-time clamping unchanged (#352).
[[nodiscard]] inline ai::Model make_full_thinking_model(std::string id) {
    auto model = make_model(std::move(id));
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::Off, "off"},
        {ai::ModelThinkingLevel::Minimal, "minimal"},
        {ai::ModelThinkingLevel::Low, "low"},
        {ai::ModelThinkingLevel::Medium, "medium"},
        {ai::ModelThinkingLevel::High, "high"},
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
        {ai::ModelThinkingLevel::Max, "max"},
    };
    return model;
}

/// Reasoning model with a caller-supplied thinking map, exercising pi's clamp
/// semantics (missing xhigh/max mapping means unsupported, a present null
/// means explicitly unsupported, other levels default to supported).
[[nodiscard]] inline ai::Model make_reasoning_model(
    std::string id,
    ai::ThinkingLevelMap map) {
    auto model = make_model(std::move(id));
    model.reasoning = true;
    model.thinking_level_map = std::move(map);
    return model;
}

} // namespace cch::tests
