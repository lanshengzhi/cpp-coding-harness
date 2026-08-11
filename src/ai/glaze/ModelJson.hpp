#pragma once

#include <cch/ai/Model.hpp>

#include "ai/ModelThinkingLevel.hpp"
#include "util/JsonGlaze.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::glaze {

using ThinkingLevelMapDto = std::map<std::string, glz::generic, std::less<>>;

struct ModelCostTierDto {
    std::optional<double> input{std::nullopt};
    std::optional<double> output{std::nullopt};
    std::optional<double> cacheRead{std::nullopt};
    std::optional<double> cacheWrite{std::nullopt};
    std::optional<std::uint64_t> inputTokensAbove{std::nullopt};
};

struct ModelCostDto {
    std::optional<double> input{std::nullopt};
    std::optional<double> output{std::nullopt};
    std::optional<double> cacheRead{std::nullopt};
    std::optional<double> cacheWrite{std::nullopt};
    std::optional<std::vector<ModelCostTierDto>> tiers{std::nullopt};
};

struct AnthropicMessagesCompatDto {
    std::optional<bool> forceAdaptiveThinking{std::nullopt};
    std::optional<bool> allowEmptySignature{std::nullopt};
};

struct ModelDto {
    std::optional<std::string> id{std::nullopt};
    std::optional<std::string> name{std::nullopt};
    std::optional<std::string> api{std::nullopt};
    std::optional<std::string> provider{std::nullopt};
    std::optional<std::string> baseUrl{std::nullopt};
    std::optional<bool> reasoning{std::nullopt};
    std::optional<ThinkingLevelMapDto> thinkingLevelMap{std::nullopt};
    std::optional<std::vector<std::string>> input{std::nullopt};
    std::optional<ModelCostDto> cost{std::nullopt};
    std::optional<std::uint64_t> contextWindow{std::nullopt};
    std::optional<std::uint64_t> maxTokens{std::nullopt};
    std::optional<ModelHeaders> headers{std::nullopt};
    std::optional<AnthropicMessagesCompatDto> compat{std::nullopt};
};

namespace detail {

template <typename T>
[[nodiscard]] inline util::Expected<T> required(
    const std::optional<T>& value,
    std::string_view field,
    std::string_view context) {
    if (value) {
        return *value;
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::JsonParse,
        "missing required Model field",
        std::string{field},
        context.empty() ? std::nullopt : std::optional<std::string>{std::string{context}}));
}

template <typename T>
[[nodiscard]] inline util::ExpectedVoid assign_required(
    T& destination,
    const std::optional<T>& value,
    std::string_view field,
    std::string_view context) {
    if (auto required_value = required(value, field, context); !required_value) {
        return std::unexpected(required_value.error());
    } else {
        destination = std::move(*required_value);
    }
    return {};
}

[[nodiscard]] inline std::string input_to_string(ModelInput input) {
    return input == ModelInput::Text ? "text" : "image";
}

[[nodiscard]] inline util::Expected<ModelInput> input_from_string(std::string_view input) {
    if (input == "text") {
        return ModelInput::Text;
    }
    if (input == "image") {
        return ModelInput::Image;
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::JsonParse,
        "invalid Model input capability",
        std::string{input}));
}

[[nodiscard]] inline std::string thinking_level_to_string(ModelThinkingLevel level) {
    if (const auto name = ai::detail::model_thinking_level_name(level)) {
        return std::string{*name};
    }
    return {};
}

[[nodiscard]] inline util::Expected<ModelThinkingLevel> thinking_level_from_string(
    std::string_view level) {
    if (const auto parsed = ai::detail::parse_model_thinking_level(level)) {
        return *parsed;
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::JsonParse,
        "invalid Model thinking level",
        std::string{level}));
}

[[nodiscard]] inline ThinkingLevelMapDto to_dto(const ThinkingLevelMap& levels) {
    ThinkingLevelMapDto dto;
    for (const auto& [level, value] : levels) {
        glz::generic serialized_value;
        if (value) {
            serialized_value = *value;
        } else {
            serialized_value = nullptr;
        }
        dto.emplace(thinking_level_to_string(level), std::move(serialized_value));
    }
    return dto;
}

[[nodiscard]] inline util::Expected<ThinkingLevelMap> from_dto(const ThinkingLevelMapDto& dto) {
    ThinkingLevelMap levels;
    for (const auto& [name, value] : dto) {
        if (auto level = thinking_level_from_string(name); !level) {
            return std::unexpected(level.error());
        } else {
            if (value.holds<std::nullptr_t>()) {
                levels.emplace(*level, std::nullopt);
                continue;
            }
            if (const auto* mapped = value.get_if<std::string>()) {
                levels.emplace(*level, *mapped);
                continue;
            }
        }
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "invalid Model thinking level mapping",
            name));
    }
    return levels;
}

[[nodiscard]] inline ModelCostTierDto to_dto(const ModelCostTier& tier) {
    return ModelCostTierDto{
        .input = tier.input,
        .output = tier.output,
        .cacheRead = tier.cache_read,
        .cacheWrite = tier.cache_write,
        .inputTokensAbove = tier.input_tokens_above,
    };
}

[[nodiscard]] inline ModelCostDto to_dto(const ModelCost& cost) {
    ModelCostDto dto{
        .input = cost.input,
        .output = cost.output,
        .cacheRead = cost.cache_read,
        .cacheWrite = cost.cache_write,
    };
    if (cost.tiers) {
        dto.tiers.emplace();
        dto.tiers->reserve(cost.tiers->size());
        for (const auto& tier : *cost.tiers) {
            dto.tiers->push_back(to_dto(tier));
        }
    }
    return dto;
}

[[nodiscard]] inline AnthropicMessagesCompatDto to_dto(const AnthropicMessagesCompat& compat) {
    return AnthropicMessagesCompatDto{
        .forceAdaptiveThinking = compat.force_adaptive_thinking,
        .allowEmptySignature = compat.allow_empty_signature,
    };
}

[[nodiscard]] inline ModelDto to_dto(const Model& model) {
    std::vector<std::string> input;
    input.reserve(model.input.size());
    for (const auto& capability : model.input) {
        input.push_back(input_to_string(capability));
    }

    ModelDto dto{
        .id = model.id,
        .name = model.name,
        .api = model.api,
        .provider = model.provider,
        .baseUrl = model.base_url,
        .reasoning = model.reasoning,
        .input = std::move(input),
        .cost = to_dto(model.cost),
        .contextWindow = model.context_window,
        .maxTokens = model.max_tokens,
        .headers = model.headers,
    };
    if (model.thinking_level_map) {
        dto.thinkingLevelMap = to_dto(*model.thinking_level_map);
    }
    if (model.compat) {
        dto.compat = to_dto(*model.compat);
    }
    return dto;
}

[[nodiscard]] inline util::Expected<ModelCostTier> tier_from_dto(
    const ModelCostTierDto& dto,
    std::string_view context) {
    double input{0};
    double output{0};
    double cache_read{0};
    double cache_write{0};
    std::uint64_t threshold{0};
    if (auto assigned = assign_required(input, dto.input, "cost.tiers[].input", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(output, dto.output, "cost.tiers[].output", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(cache_read, dto.cacheRead, "cost.tiers[].cacheRead", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(cache_write, dto.cacheWrite, "cost.tiers[].cacheWrite", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(
            threshold,
            dto.inputTokensAbove,
            "cost.tiers[].inputTokensAbove",
            context);
        !assigned) {
        return std::unexpected(assigned.error());
    }
    return ModelCostTier{
        .input = input,
        .output = output,
        .cache_read = cache_read,
        .cache_write = cache_write,
        .input_tokens_above = threshold,
    };
}

[[nodiscard]] inline util::Expected<ModelCost> cost_from_dto(
    const ModelCostDto& dto,
    std::string_view context) {
    ModelCost cost;
    if (auto assigned = assign_required(cost.input, dto.input, "cost.input", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(cost.output, dto.output, "cost.output", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(cost.cache_read, dto.cacheRead, "cost.cacheRead", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(cost.cache_write, dto.cacheWrite, "cost.cacheWrite", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (dto.tiers) {
        cost.tiers.emplace();
        cost.tiers->reserve(dto.tiers->size());
        for (const auto& tier_dto : *dto.tiers) {
            if (auto tier = tier_from_dto(tier_dto, context); !tier) {
                return std::unexpected(tier.error());
            } else {
                cost.tiers->push_back(std::move(*tier));
            }
        }
    }
    return cost;
}

[[nodiscard]] inline util::Expected<Model> from_dto(const ModelDto& dto, std::string_view context) {
    std::string id;
    std::string name;
    std::string api;
    std::string provider;
    std::string base_url;
    bool reasoning{false};
    std::vector<std::string> inputs;
    ModelCostDto cost_dto;
    std::uint64_t context_window{0};
    std::uint64_t max_tokens{0};
    if (auto assigned = assign_required(id, dto.id, "id", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(name, dto.name, "name", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(api, dto.api, "api", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(provider, dto.provider, "provider", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(base_url, dto.baseUrl, "baseUrl", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(reasoning, dto.reasoning, "reasoning", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(inputs, dto.input, "input", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(cost_dto, dto.cost, "cost", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(context_window, dto.contextWindow, "contextWindow", context); !assigned) {
        return std::unexpected(assigned.error());
    }
    if (auto assigned = assign_required(max_tokens, dto.maxTokens, "maxTokens", context); !assigned) {
        return std::unexpected(assigned.error());
    }

    std::vector<ModelInput> input;
    input.reserve(inputs.size());
    for (const auto& value : inputs) {
        if (auto capability = input_from_string(value); !capability) {
            return std::unexpected(capability.error());
        } else {
            input.push_back(*capability);
        }
    }
    ModelCost cost;
    if (auto parsed_cost = cost_from_dto(cost_dto, context); !parsed_cost) {
        return std::unexpected(parsed_cost.error());
    } else {
        cost = std::move(*parsed_cost);
    }

    std::optional<ThinkingLevelMap> thinking_level_map;
    if (dto.thinkingLevelMap) {
        if (auto levels = from_dto(*dto.thinkingLevelMap); !levels) {
            return std::unexpected(levels.error());
        } else {
            thinking_level_map = std::move(*levels);
        }
    }

    Model model{
        .id = std::move(id),
        .name = std::move(name),
        .api = std::move(api),
        .provider = std::move(provider),
        .base_url = std::move(base_url),
        .reasoning = reasoning,
        .thinking_level_map = std::move(thinking_level_map),
        .input = std::move(input),
        .cost = std::move(cost),
        .context_window = context_window,
        .max_tokens = max_tokens,
        .headers = dto.headers,
        .compat = dto.compat
            ? std::optional<AnthropicMessagesCompat>{AnthropicMessagesCompat{
                  .force_adaptive_thinking = dto.compat->forceAdaptiveThinking,
                  .allow_empty_signature = dto.compat->allowEmptySignature,
              }}
            : std::nullopt,
    };
    if (auto valid = validate_model(model); !valid) {
        return std::unexpected(valid.error());
    }
    return model;
}

} // namespace detail

[[nodiscard]] inline util::Expected<std::string> write_model_json(const Model& model) {
    if (auto valid = validate_model(model); !valid) {
        return std::unexpected(valid.error());
    }
    return util::write_json(detail::to_dto(model));
}

[[nodiscard]] inline util::Expected<Model> read_model_json(std::string_view json) {
    if (auto dto = util::read_json<ModelDto>(json); !dto) {
        return std::unexpected(dto.error());
    } else {
        return detail::from_dto(*dto, json);
    }
}

} // namespace cch::ai::glaze
