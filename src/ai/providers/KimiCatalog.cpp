#include "KimiCatalog.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai::providers {
namespace {

[[nodiscard]] Model kimi_model(
    std::string id,
    std::string name,
    ModelCost cost,
    std::uint64_t context_window,
    std::uint64_t max_tokens,
    std::optional<ThinkingLevelMap> thinking_level_map,
    bool allow_empty_signature) {
    return Model{
        .id = std::move(id),
        .name = std::move(name),
        .api = "anthropic-messages",
        .provider = "kimi-coding",
        .base_url = "https://api.kimi.com/coding",
        .reasoning = true,
        .thinking_level_map = std::move(thinking_level_map),
        .input = {ModelInput::Text, ModelInput::Image},
        .cost = std::move(cost),
        .context_window = context_window,
        .max_tokens = max_tokens,
        .headers = ModelHeaders{{"User-Agent", "KimiCLI/1.5"}},
        .compat = AnthropicMessagesCompat{
            .force_adaptive_thinking = true,
            .allow_empty_signature = allow_empty_signature
                ? std::optional<bool>{true}
                : std::nullopt,
        },
    };
}

[[nodiscard]] ThinkingLevelMap k3_thinking_levels() {
    return ThinkingLevelMap{
        {ModelThinkingLevel::Off, std::nullopt},
        {ModelThinkingLevel::Minimal, std::nullopt},
        {ModelThinkingLevel::Low, "low"},
        {ModelThinkingLevel::Medium, std::nullopt},
        {ModelThinkingLevel::High, "high"},
        {ModelThinkingLevel::XHigh, std::nullopt},
        {ModelThinkingLevel::Max, "max"},
    };
}

} // namespace

std::vector<Model> kimi_coding_models() {
    std::vector<Model> models;
    models.push_back(kimi_model(
        "k3",
        "Kimi K3",
        ModelCost{
            .input = 3,
            .output = 15,
            .cache_read = 0.3,
            .cache_write = 0,
        },
        1'048'576,
        131'072,
        k3_thinking_levels(),
        true));
    models.push_back(kimi_model(
        "k3-256k",
        "Kimi K3-256K",
        ModelCost{},
        262'144,
        131'072,
        k3_thinking_levels(),
        false));
    models.push_back(kimi_model(
        "kimi-for-coding",
        "Kimi K2.7 Code",
        ModelCost{
            .input = 0.95,
            .output = 4,
            .cache_read = 0.19,
            .cache_write = 0,
        },
        262'144,
        32'768,
        std::nullopt,
        true));
    models.push_back(kimi_model(
        "kimi-for-coding-highspeed",
        "Kimi For Coding HighSpeed",
        ModelCost{
            .input = 1.9,
            .output = 8,
            .cache_read = 0.38,
            .cache_write = 0,
        },
        262'144,
        32'768,
        std::nullopt,
        false));
    return models;
}

} // namespace cch::ai::providers
