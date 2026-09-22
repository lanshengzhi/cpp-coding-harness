#include <cch/ai/Model.hpp>

#include <cch/support/Error.hpp>

#include <cmath>
#include <format>
#include <string_view>
#include <type_traits>

namespace cch::ai {
namespace {

[[nodiscard]] support::ExpectedVoid validate_identity(std::string_view value, std::string_view field) {
    if (!value.empty()) {
        return {};
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "invalid model",
        std::format("{} must not be empty", field)));
}

[[nodiscard]] support::ExpectedVoid validate_rate(double value, std::string_view field) {
    if (std::isfinite(value) && value >= 0) {
        return {};
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
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

[[nodiscard]] support::ExpectedVoid validate_rates(
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

support::ExpectedVoid validate_model(const Model& model) {
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
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "invalid model input capability"));
        }
    }
    if (model.thinking_level_map) {
        for (const auto& [level, _] : *model.thinking_level_map) {
            if (!model_thinking_level_name(level)) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Validation,
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
    if (model.compat) {
        const auto [compat_name, expected_api] = std::visit(
                [](const auto& compat) -> std::pair<std::string_view, std::string_view> {
                    using Compat = std::decay_t<decltype(compat)>;
                    if constexpr (std::is_same_v<Compat, AnthropicMessagesCompat>) {
                        return {"AnthropicMessagesCompat", "anthropic-messages"};
                    } else if constexpr (std::is_same_v<Compat, OpenAICompletionsCompat>) {
                        return {"OpenAICompletionsCompat", "openai-completions"};
                    } else {
                        return {"OpenAIResponsesCompat", "openai-responses"};
                    }
                },
                *model.compat);
        if (model.api != expected_api) {
            return std::unexpected(support::make_error(support::ErrorCode::ModelValidation,
                    "invalid model compat",
                    std::format("{} requires api '{}', but model api is '{}'", compat_name, expected_api, model.api)));
        }
    }
    return {};
}

} // namespace cch::ai
