#include <cch/ai/Model.hpp>

#include <cch/util/Error.hpp>

#include "ai/ModelThinkingLevel.hpp"

#include <cmath>
#include <format>
#include <string_view>

namespace cch::ai {
namespace {

[[nodiscard]] util::ExpectedVoid validate_identity(std::string_view value, std::string_view field) {
    if (!value.empty()) {
        return {};
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "invalid model",
        std::format("{} must not be empty", field)));
}

[[nodiscard]] util::ExpectedVoid validate_rate(double value, std::string_view field) {
    if (std::isfinite(value) && value >= 0) {
        return {};
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "invalid model cost",
        std::format("{} must be finite and non-negative", field)));
}

[[nodiscard]] bool is_valid_input(ModelInput input) {
    switch (input) {
    case ModelInput::Text:
    case ModelInput::Image:
        return true;
    }
    return false;
}

[[nodiscard]] util::ExpectedVoid validate_rates(
    double input,
    double output,
    double cache_read,
    double cache_write,
    std::string_view prefix) {
    if (auto result = validate_rate(input, std::format("{}.input", prefix)); !result) {
        return result;
    }
    if (auto result = validate_rate(output, std::format("{}.output", prefix)); !result) {
        return result;
    }
    if (auto result = validate_rate(cache_read, std::format("{}.cacheRead", prefix)); !result) {
        return result;
    }
    return validate_rate(cache_write, std::format("{}.cacheWrite", prefix));
}

} // namespace

util::ExpectedVoid validate_model(const Model& model) {
    if (auto result = validate_identity(model.id, "id"); !result) {
        return result;
    }
    if (auto result = validate_identity(model.name, "name"); !result) {
        return result;
    }
    if (auto result = validate_identity(model.api, "api"); !result) {
        return result;
    }
    if (auto result = validate_identity(model.provider, "provider"); !result) {
        return result;
    }
    if (auto result = validate_rates(
            model.cost.input,
            model.cost.output,
            model.cost.cache_read,
            model.cost.cache_write,
            "cost");
        !result) {
        return result;
    }
    for (const auto& input : model.input) {
        if (!is_valid_input(input)) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid model input capability"));
        }
    }
    if (model.thinking_level_map) {
        for (const auto& [level, _] : *model.thinking_level_map) {
            if (!detail::model_thinking_level_name(level)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    "invalid model thinking level"));
            }
        }
    }
    if (model.cost.tiers) {
        for (const auto& tier : *model.cost.tiers) {
            if (auto result = validate_rates(
                    tier.input,
                    tier.output,
                    tier.cache_read,
                    tier.cache_write,
                    "cost.tiers");
                !result) {
                return result;
            }
        }
    }
    if (model.compat && model.api != "anthropic-messages") {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid model compat",
            "AnthropicMessagesCompat requires api 'anthropic-messages'"));
    }
    return {};
}

} // namespace cch::ai
