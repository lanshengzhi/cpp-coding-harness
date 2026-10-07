#include <cch/ai/Model.hpp>

#include <cch/support/Error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
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
    // pi uses -1,000,000 for provider-routed models whose price is not
    // published by the provider (OpenRouter's auto/auto-beta entries).
    constexpr double kProviderRoutedCost = -1'000'000.0;
    if (std::isfinite(value) && (value >= 0 || value == kProviderRoutedCost)) {
        return {};
    }
    return std::unexpected(support::make_error(support::ErrorCode::Validation,
            "invalid model cost",
            std::format("{} must be finite, non-negative, or the provider-routed sentinel", field)));
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
        // The Responses compat shape is shared by two catalog API identities:
        // `openai-responses` and `openai-codex-responses` both carry the
        // grammar-tool flag (ADR 0033's #885 amendment), so the Responses
        // alternative accepts either. The other alternatives pin exactly one
        // api.
        static constexpr std::array<std::string_view, 1> kAnthropicApis{"anthropic-messages"};
        static constexpr std::array<std::string_view, 1> kCompletionsApis{"openai-completions"};
        static constexpr std::array<std::string_view, 2> kResponsesApis{"openai-responses", "openai-codex-responses"};
        const auto [compat_name, expected_apis] = std::visit(
                [](const auto& compat) -> std::pair<std::string_view, std::span<const std::string_view>> {
                    using Compat = std::decay_t<decltype(compat)>;
                    if constexpr (std::is_same_v<Compat, AnthropicMessagesCompat>) {
                        return {"AnthropicMessagesCompat", kAnthropicApis};
                    } else if constexpr (std::is_same_v<Compat, OpenAICompletionsCompat>) {
                        return {"OpenAICompletionsCompat", kCompletionsApis};
                    } else {
                        return {"OpenAIResponsesCompat", kResponsesApis};
                    }
                },
                *model.compat);
        if (std::ranges::find(expected_apis, model.api) == expected_apis.end()) {
            return std::unexpected(support::make_error(support::ErrorCode::ModelValidation,
                    "invalid model compat",
                    std::format("{} requires api '{}', but model api is '{}'",
                            compat_name,
                            expected_apis.front(),
                            model.api)));
        }
    }
    return {};
}

} // namespace cch::ai
