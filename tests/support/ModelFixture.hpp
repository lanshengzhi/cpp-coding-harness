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

} // namespace cch::tests
