#include "CodexCatalog.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai::providers {
namespace {

[[nodiscard]] Model codex_model(
    std::string id,
    std::string name,
    ModelCost cost,
    std::optional<ThinkingLevelMap> thinking_level_map) {
    return Model{
        .id = std::move(id),
        .name = std::move(name),
        .api = "openai-codex-responses",
        .provider = "openai-codex",
        .base_url = "https://chatgpt.com/backend-api",
        .reasoning = true,
        .thinking_level_map = std::move(thinking_level_map),
        .input = {ModelInput::Text, ModelInput::Image},
        .cost = std::move(cost),
        .context_window = 272000,
        .max_tokens = 128000,
        .headers = std::nullopt,
        .compat = std::nullopt,
    };
}

[[nodiscard]] ThinkingLevelMap codex_thinking_levels() {
    return ThinkingLevelMap{
        {ModelThinkingLevel::Minimal, "low"},
        {ModelThinkingLevel::XHigh, "xhigh"},
    };
}

[[nodiscard]] ThinkingLevelMap codex_thinking_levels_with_max() {
    return ThinkingLevelMap{
        {ModelThinkingLevel::Minimal, "low"},
        {ModelThinkingLevel::XHigh, "xhigh"},
        {ModelThinkingLevel::Max, "max"},
    };
}

} // namespace

std::vector<Model> codex_models() {
    std::vector<Model> models;
    // gpt-5.3-codex-spark (text only, 128k context, no tiers).
    models.push_back(Model{
        .id = "gpt-5.3-codex-spark",
        .name = "GPT-5.3 Codex Spark",
        .api = "openai-codex-responses",
        .provider = "openai-codex",
        .base_url = "https://chatgpt.com/backend-api",
        .reasoning = true,
        .thinking_level_map = codex_thinking_levels(),
        .input = {ModelInput::Text},
        .cost = ModelCost{
            .input = 1.75,
            .output = 14,
            .cache_read = 0.175,
            .cache_write = 0,
        },
        .context_window = 128000,
        .max_tokens = 128000,
        .headers = std::nullopt,
        .compat = std::nullopt,
    });
    models.push_back(codex_model(
        "gpt-5.4",
        "GPT-5.4",
        ModelCost{
            .input = 2.5,
            .output = 15,
            .cache_read = 0.25,
            .cache_write = 0,
            .tiers = std::vector<ModelCostTier>{
                ModelCostTier{
                    .input = 5,
                    .output = 22.5,
                    .cache_read = 0.5,
                    .cache_write = 0,
                    .input_tokens_above = 272000,
                },
            },
        },
        codex_thinking_levels()));
    models.push_back(codex_model(
        "gpt-5.4-mini",
        "GPT-5.4 mini",
        ModelCost{
            .input = 0.75,
            .output = 4.5,
            .cache_read = 0.075,
            .cache_write = 0,
        },
        codex_thinking_levels()));
    models.push_back(codex_model(
        "gpt-5.5",
        "GPT-5.5",
        ModelCost{
            .input = 5,
            .output = 30,
            .cache_read = 0.5,
            .cache_write = 0,
            .tiers = std::vector<ModelCostTier>{
                ModelCostTier{
                    .input = 10,
                    .output = 45,
                    .cache_read = 1,
                    .cache_write = 0,
                    .input_tokens_above = 272000,
                },
            },
        },
        codex_thinking_levels()));
    models.push_back(codex_model(
        "gpt-5.6-luna",
        "GPT-5.6 Luna",
        ModelCost{
            .input = 0.2,
            .output = 1.2,
            .cache_read = 0.02,
            .cache_write = 0.25,
            .tiers = std::vector<ModelCostTier>{
                ModelCostTier{
                    .input = 0.4,
                    .output = 1.8,
                    .cache_read = 0.04,
                    .cache_write = 0.5,
                    .input_tokens_above = 272000,
                },
            },
        },
        codex_thinking_levels_with_max()));
    models.push_back(codex_model(
        "gpt-5.6-sol",
        "GPT-5.6 Sol",
        ModelCost{
            .input = 5,
            .output = 30,
            .cache_read = 0.5,
            .cache_write = 6.25,
            .tiers = std::vector<ModelCostTier>{
                ModelCostTier{
                    .input = 10,
                    .output = 45,
                    .cache_read = 1,
                    .cache_write = 12.5,
                    .input_tokens_above = 272000,
                },
            },
        },
        codex_thinking_levels_with_max()));
    models.push_back(codex_model(
        "gpt-5.6-terra",
        "GPT-5.6 Terra",
        ModelCost{
            .input = 2,
            .output = 12,
            .cache_read = 0.2,
            .cache_write = 2.5,
            .tiers = std::vector<ModelCostTier>{
                ModelCostTier{
                    .input = 4,
                    .output = 18,
                    .cache_read = 0.4,
                    .cache_write = 5,
                    .input_tokens_above = 272000,
                },
            },
        },
        codex_thinking_levels_with_max()));
    return models;
}

} // namespace cch::ai::providers
