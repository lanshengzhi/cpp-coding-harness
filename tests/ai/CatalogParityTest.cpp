#include <cch/ai/Model.hpp>
#include <cch/ai/Models.hpp>

#include "support/PiFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using JsonValue = support::JsonValue;
using Object = JsonValue::object_t;
using ParseError = std::string;

template <typename T> using ParseResult = std::expected<T, ParseError>;

constexpr std::array<std::string_view, 6> kUpstreamProviders{
        "deepseek",
        "kimi-coding",
        "openai",
        "openai-codex",
        "openrouter",
        "opencode-go",
};

constexpr std::array<std::string_view, 5> kMirrorProviders{
        "deepseek",
        "openai",
        "openai-codex",
        "openrouter",
        "opencode-go",
};

constexpr std::array<std::string_view, 13> kCarriedModelFields{
        "id",
        "name",
        "api",
        "provider",
        "baseUrl",
        "reasoning",
        "thinkingLevelMap",
        "input",
        "cost",
        "contextWindow",
        "maxTokens",
        "headers",
        "compat",
};

// These are the only raw fields intentionally absent from Model. Their names
// are kept here so a new upstream field cannot disappear through a permissive
// parser. inputLimits is deferred as a complete nested shape:
// maxRequestBytes, maxPerRequest, images.maxPerRequest, and
// images.resize.{maxWidth,maxHeight,maxBytes,jpegQuality}.
constexpr std::array<std::string_view, 2> kDeferredInputLimitLeaves{
        "maxRequestBytes",
        "maxPerRequest",
};

constexpr std::array<std::string_view, 4> kDeferredResizeLeaves{
        "maxWidth",
        "maxHeight",
        "maxBytes",
        "jpegQuality",
};

// T1 carries only the typed fields represented by Model.hpp. The remaining
// catalog flags are explicit Deferred fields: grammar/tool-search/additional
// tools, mid-conversation system/tool additions, session-affinity request
// flags and effort detection. Strict mode is carried for both Responses and
// Completions; grammar-constrained tool schemas remain deferred.
constexpr std::array<std::string_view, 7> kDeferredCompatFields{
        "supportsOpenAIGrammarTools",
        "supportsToolSearch",
        "supportsAdditionalTools",
        "supportsMidConvoSystemMessages",
        "supportsMidConvoToolAdditions",
        "sendSessionAffinityHeaders",
        "supportsMidConvoEffort",
};

constexpr std::array<std::string_view, 17> kBooleanCompatFields{
        "supportsStore",
        "supportsDeveloperRole",
        "requiresReasoningContentOnAssistantMessages",
        "supportsLongCacheRetention",
        "supportsReasoningEffort",
        "forceAdaptiveThinking",
        "allowEmptySignature",
        "supportsTemperature",
        "supportsStrictMode",
        "supportsExplicitPromptCacheMode",
        "supportsOpenAIGrammarTools",
        "supportsToolSearch",
        "supportsAdditionalTools",
        "supportsMidConvoSystemMessages",
        "supportsMidConvoToolAdditions",
        "sendSessionAffinityHeaders",
        "supportsMidConvoEffort",
};

constexpr std::array<std::string_view, 4> kStringCompatFields{
        "maxTokensField",
        "thinkingFormat",
        "cacheControlFormat",
        "sessionAffinityFormat",
};

template <std::size_t N>
[[nodiscard]] bool contains(const std::array<std::string_view, N>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

template <typename T> [[nodiscard]] ParseResult<T> fail(std::string path, std::string message) {
    return std::unexpected(std::move(path) + ": " + std::move(message));
}

[[nodiscard]] const JsonValue* member(const Object& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] ParseResult<const JsonValue*> required_member(
        const Object& object, std::string_view key, std::string_view path) {
    if (const auto* value = member(object, key)) {
        return value;
    }
    return fail<const JsonValue*>(std::string{path} + "." + std::string{key}, "missing field");
}

[[nodiscard]] ParseResult<const std::string*> required_string(const JsonValue& value, std::string path) {
    if (const auto* text = value.get_if<std::string>()) {
        return text;
    }
    return fail<const std::string*>(std::move(path), "expected string");
}

[[nodiscard]] ParseResult<const Object*> required_object(const JsonValue& value, std::string path) {
    if (const auto* object = value.get_if<Object>()) {
        return object;
    }
    return fail<const Object*>(std::move(path), "expected object");
}

[[nodiscard]] ParseResult<const JsonValue::array_t*> required_array(const JsonValue& value, std::string path) {
    if (const auto* array = value.get_if<JsonValue::array_t>()) {
        return array;
    }
    return fail<const JsonValue::array_t*>(std::move(path), "expected array");
}

[[nodiscard]] ParseResult<bool> required_bool(const JsonValue& value, std::string path) {
    if (const auto* flag = value.get_if<bool>()) {
        return *flag;
    }
    return fail<bool>(std::move(path), "expected boolean");
}

[[nodiscard]] ParseResult<double> required_number(const JsonValue& value, std::string path) {
    if (const auto* number = value.get_if<double>(); number != nullptr && std::isfinite(*number)) {
        return *number;
    }
    return fail<double>(std::move(path), "expected finite number");
}

[[nodiscard]] ParseResult<std::uint64_t> required_uint64(const JsonValue& value, std::string path) {
    auto number = required_number(value, path);
    if (!number) {
        return std::unexpected(number.error());
    }
    if (*number < 0 || std::floor(*number) != *number ||
            *number > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return fail<std::uint64_t>(std::move(path), "expected non-negative integer");
    }
    return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] ParseResult<const std::string*> required_string_member(
        const Object& object, std::string_view key, std::string_view path) {
    auto value = required_member(object, key, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return required_string(**value, std::string{path} + "." + std::string{key});
}

[[nodiscard]] ParseResult<std::optional<bool>> optional_scalar_bool(
        const Object& object, std::string_view key, std::string_view path) {
    const auto* value = member(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    auto parsed = required_bool(*value, std::string{path} + "." + std::string{key});
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::optional<bool>{*parsed};
}

[[nodiscard]] ParseResult<std::optional<std::string>> optional_scalar_string(
        const Object& object, std::string_view key, std::string_view path) {
    const auto* value = member(object, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    auto parsed = required_string(*value, std::string{path} + "." + std::string{key});
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::optional<std::string>{**parsed};
}

[[nodiscard]] ParseResult<void> audit_input_limits(const JsonValue& value, std::string path) {
    auto input_limits = required_object(value, path);
    if (!input_limits) {
        return std::unexpected(input_limits.error());
    }

    for (const auto& [key, child] : **input_limits) {
        if (contains(kDeferredInputLimitLeaves, key)) {
            auto number = required_number(child, std::format("{}.{}", path, key));
            if (!number) {
                return std::unexpected(number.error());
            }
            continue;
        }
        if (key != "images") {
            return fail<void>(std::format("{}.{}", path, key), "unknown Deferred inputLimits field");
        }

        auto images = required_object(child, std::format("{}.images", path));
        if (!images) {
            return std::unexpected(images.error());
        }
        for (const auto& [image_key, image_child] : **images) {
            if (image_key == "maxPerRequest") {
                auto number = required_number(image_child, std::format("{}.images.maxPerRequest", path));
                if (!number) {
                    return std::unexpected(number.error());
                }
                continue;
            }
            if (image_key != "resize") {
                return fail<void>(std::format("{}.images.{}", path, image_key), "unknown Deferred image-limit field");
            }

            auto resize = required_object(image_child, std::format("{}.images.resize", path));
            if (!resize) {
                return std::unexpected(resize.error());
            }
            for (const auto& [resize_key, resize_child] : **resize) {
                if (!contains(kDeferredResizeLeaves, resize_key)) {
                    return fail<void>(
                            std::format("{}.images.resize.{}", path, resize_key), "unknown Deferred resize field");
                }
                auto number = required_number(resize_child, std::format("{}.images.resize.{}", path, resize_key));
                if (!number) {
                    return std::unexpected(number.error());
                }
            }
        }
    }
    return {};
}

[[nodiscard]] ParseResult<void> audit_cost(const JsonValue& value, std::string path) {
    auto cost = required_object(value, path);
    if (!cost) {
        return std::unexpected(cost.error());
    }
    constexpr std::array<std::string_view, 4> kRates{
            "input",
            "output",
            "cacheRead",
            "cacheWrite",
    };

    for (const auto& [key, child] : **cost) {
        if (contains(kRates, key)) {
            auto number = required_number(child, std::format("{}.{}", path, key));
            if (!number) {
                return std::unexpected(number.error());
            }
            continue;
        }
        if (key != "tiers") {
            return fail<void>(std::format("{}.{}", path, key), "unknown cost field");
        }

        auto tiers = required_array(child, std::format("{}.tiers", path));
        if (!tiers) {
            return std::unexpected(tiers.error());
        }
        constexpr std::array<std::string_view, 5> kTierFields{
                "input",
                "output",
                "cacheRead",
                "cacheWrite",
                "inputTokensAbove",
        };
        for (std::size_t index = 0; index < (*tiers)->size(); ++index) {
            const auto tier_path = std::format("{}.tiers[{}]", path, index);
            auto tier = required_object((*tiers)->at(index), tier_path);
            if (!tier) {
                return std::unexpected(tier.error());
            }
            for (const auto& [tier_key, tier_child] : **tier) {
                if (!contains(kTierFields, tier_key)) {
                    return fail<void>(std::format("{}.{}", tier_path, tier_key), "unknown cost tier field");
                }
                auto number = required_number(tier_child, std::format("{}.{}", tier_path, tier_key));
                if (!number) {
                    return std::unexpected(number.error());
                }
            }
        }
    }
    return {};
}

[[nodiscard]] ParseResult<void> audit_thinking_map(const JsonValue& value, std::string path) {
    auto map = required_object(value, path);
    if (!map) {
        return std::unexpected(map.error());
    }
    for (const auto& [key, child] : **map) {
        if (!ai::parse_model_thinking_level(key)) {
            return fail<void>(std::format("{}.{}", path, key), "unknown thinking level");
        }
        if (!child.holds<JsonValue::null_t>() && child.get_if<std::string>() == nullptr) {
            return fail<void>(std::format("{}.{}", path, key), "expected string or JSON null");
        }
    }
    return {};
}

[[nodiscard]] ParseResult<void> audit_input(const JsonValue& value, std::string path) {
    auto input = required_array(value, path);
    if (!input) {
        return std::unexpected(input.error());
    }
    for (std::size_t index = 0; index < (*input)->size(); ++index) {
        auto text = required_string((*input)->at(index), std::format("{}[{}]", path, index));
        if (!text) {
            return std::unexpected(text.error());
        }
        if (**text != "text" && **text != "image") {
            return fail<void>(std::format("{}[{}]", path, index), "unknown input modality");
        }
    }
    return {};
}

[[nodiscard]] bool carried_compat_field(std::string_view api, std::string_view key) {
    if (api == "openai-completions") {
        return contains(
                std::array<std::string_view, 9>{
                        "supportsStore",
                        "supportsDeveloperRole",
                        "supportsStrictMode",
                        "maxTokensField",
                        "requiresReasoningContentOnAssistantMessages",
                        "thinkingFormat",
                        "cacheControlFormat",
                        "supportsLongCacheRetention",
                        "supportsReasoningEffort",
                },
                key);
    }
    if (api == "openai-responses") {
        return contains(
                std::array<std::string_view, 3>{
                        "sessionAffinityFormat",
                        "supportsStrictMode",
                        "supportsExplicitPromptCacheMode",
                },
                key);
    }
    if (api == "anthropic-messages") {
        return contains(
                std::array<std::string_view, 3>{
                        "forceAdaptiveThinking",
                        "allowEmptySignature",
                        "supportsTemperature",
                },
                key);
    }
    return false;
}

[[nodiscard]] bool deferred_compat_field(std::string_view key) {
    if (contains(kDeferredCompatFields, key)) {
        return true;
    }
    return false;
}

[[nodiscard]] ParseResult<void> audit_compat(std::string_view api, const JsonValue& value, std::string path) {
    auto compat = required_object(value, path);
    if (!compat) {
        return std::unexpected(compat.error());
    }
    for (const auto& [key, child] : **compat) {
        if (!carried_compat_field(api, key) && !deferred_compat_field(key)) {
            return fail<void>(std::format("{}.{}", path, key), "unknown compatibility field");
        }
        if (contains(kBooleanCompatFields, key)) {
            auto flag = required_bool(child, std::format("{}.{}", path, key));
            if (!flag) {
                return std::unexpected(flag.error());
            }
        } else if (contains(kStringCompatFields, key)) {
            auto text = required_string(child, std::format("{}.{}", path, key));
            if (!text) {
                return std::unexpected(text.error());
            }
        } else {
            return fail<void>(std::format("{}.{}", path, key), "compatibility field has no declared scalar type");
        }
    }
    return {};
}

[[nodiscard]] ParseResult<void> audit_model_fields(std::string_view api, const Object& model, std::string path) {
    for (const auto& [key, value] : model) {
        if (!contains(kCarriedModelFields, key) && key != "inputLimits") {
            return fail<void>(std::format("{}.{}", path, key), "unknown raw model field");
        }
        if (key == "inputLimits") {
            auto result = audit_input_limits(value, std::format("{}.inputLimits", path));
            if (!result) {
                return result;
            }
        } else if (key == "cost") {
            auto result = audit_cost(value, std::format("{}.cost", path));
            if (!result) {
                return result;
            }
        } else if (key == "thinkingLevelMap") {
            auto result = audit_thinking_map(value, std::format("{}.thinkingLevelMap", path));
            if (!result) {
                return result;
            }
        } else if (key == "input") {
            auto result = audit_input(value, std::format("{}.input", path));
            if (!result) {
                return result;
            }
        } else if (key == "headers") {
            auto headers = required_object(value, std::format("{}.headers", path));
            if (!headers) {
                return std::unexpected(headers.error());
            }
            for (const auto& [header_name, header_value] : **headers) {
                auto text = required_string(header_value, std::format("{}.headers.{}", path, header_name));
                if (!text) {
                    return std::unexpected(text.error());
                }
            }
        } else if (key == "compat") {
            auto result = audit_compat(api, value, std::format("{}.compat", path));
            if (!result) {
                return result;
            }
        }
    }
    return {};
}

[[nodiscard]] ParseResult<ai::ModelInput> parse_input_modality(std::string_view value, std::string path) {
    if (value == "text") {
        return ai::ModelInput::Text;
    }
    if (value == "image") {
        return ai::ModelInput::Image;
    }
    return fail<ai::ModelInput>(std::move(path), "unknown input modality");
}

[[nodiscard]] ParseResult<ai::ModelCost> parse_cost(const JsonValue& value, std::string path) {
    auto object = required_object(value, path);
    if (!object) {
        return std::unexpected(object.error());
    }
    ai::ModelCost cost;
    for (const auto& [key, target] : std::array{
                 std::pair<std::string_view, double*>("input", &cost.input),
                 std::pair<std::string_view, double*>("output", &cost.output),
                 std::pair<std::string_view, double*>("cacheRead", &cost.cache_read),
                 std::pair<std::string_view, double*>("cacheWrite", &cost.cache_write),
         }) {
        auto value_member = required_member(**object, key, path);
        if (!value_member) {
            return std::unexpected(value_member.error());
        }
        auto number = required_number(**value_member, std::format("{}.{}", path, key));
        if (!number) {
            return std::unexpected(number.error());
        }
        *target = *number;
    }

    if (const auto* tiers_value = member(**object, "tiers")) {
        auto tiers = required_array(*tiers_value, std::format("{}.tiers", path));
        if (!tiers) {
            return std::unexpected(tiers.error());
        }
        std::vector<ai::ModelCostTier> parsed;
        parsed.reserve((*tiers)->size());
        for (std::size_t index = 0; index < (*tiers)->size(); ++index) {
            const auto tier_path = std::format("{}.tiers[{}]", path, index);
            auto tier_object = required_object((*tiers)->at(index), tier_path);
            if (!tier_object) {
                return std::unexpected(tier_object.error());
            }
            ai::ModelCostTier tier;
            for (const auto& [key, target] : std::array{
                         std::pair<std::string_view, double*>("input", &tier.input),
                         std::pair<std::string_view, double*>("output", &tier.output),
                         std::pair<std::string_view, double*>("cacheRead", &tier.cache_read),
                         std::pair<std::string_view, double*>("cacheWrite", &tier.cache_write),
                 }) {
                auto value_member = required_member(**tier_object, key, tier_path);
                if (!value_member) {
                    return std::unexpected(value_member.error());
                }
                auto number = required_number(**value_member, std::format("{}.{}", tier_path, key));
                if (!number) {
                    return std::unexpected(number.error());
                }
                *target = *number;
            }
            auto input_tokens = required_member(**tier_object, "inputTokensAbove", tier_path);
            if (!input_tokens) {
                return std::unexpected(input_tokens.error());
            }
            auto parsed_input_tokens = required_uint64(**input_tokens, std::format("{}.inputTokensAbove", tier_path));
            if (!parsed_input_tokens) {
                return std::unexpected(parsed_input_tokens.error());
            }
            tier.input_tokens_above = *parsed_input_tokens;
            parsed.push_back(std::move(tier));
        }
        cost.tiers = std::move(parsed);
    }
    return cost;
}

[[nodiscard]] ParseResult<std::optional<ai::ThinkingLevelMap>> parse_thinking_map(
        const Object& model, std::string path) {
    const auto* value = member(model, "thinkingLevelMap");
    if (value == nullptr) {
        return std::nullopt;
    }
    auto object = required_object(*value, std::format("{}.thinkingLevelMap", path));
    if (!object) {
        return std::unexpected(object.error());
    }
    ai::ThinkingLevelMap map;
    for (const auto& [key, child] : **object) {
        const auto level = ai::parse_model_thinking_level(key);
        if (!level) {
            return fail<std::optional<ai::ThinkingLevelMap>>(
                    std::format("{}.thinkingLevelMap.{}", path, key), "unknown thinking level");
        }
        if (child.holds<JsonValue::null_t>()) {
            map.emplace(*level, std::nullopt);
        } else if (const auto* text = child.get_if<std::string>()) {
            map.emplace(*level, *text);
        } else {
            return fail<std::optional<ai::ThinkingLevelMap>>(
                    std::format("{}.thinkingLevelMap.{}", path, key), "expected string or JSON null");
        }
    }
    return std::optional<ai::ThinkingLevelMap>{std::move(map)};
}

[[nodiscard]] ParseResult<std::optional<ai::ModelHeaders>> parse_headers(const Object& model, std::string path) {
    const auto* value = member(model, "headers");
    if (value == nullptr) {
        return std::nullopt;
    }
    auto object = required_object(*value, std::format("{}.headers", path));
    if (!object) {
        return std::unexpected(object.error());
    }
    ai::ModelHeaders headers;
    for (const auto& [key, child] : **object) {
        auto text = required_string(child, std::format("{}.headers.{}", path, key));
        if (!text) {
            return std::unexpected(text.error());
        }
        headers.emplace(key, **text);
    }
    return std::optional<ai::ModelHeaders>{std::move(headers)};
}

[[nodiscard]] ParseResult<std::optional<ai::ModelCompatVariant>> parse_compat(
        std::string_view api, const Object& model, std::string path) {
    const auto* value = member(model, "compat");
    if (value == nullptr) {
        return std::nullopt;
    }
    auto object = required_object(*value, std::format("{}.compat", path));
    if (!object) {
        return std::unexpected(object.error());
    }

    if (api == "anthropic-messages") {
        ai::AnthropicMessagesCompat compat;
        bool populated = false;
        for (const auto& [key, target] : std::array{
                     std::pair<std::string_view, std::optional<bool>*>(
                             "forceAdaptiveThinking", &compat.force_adaptive_thinking),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "allowEmptySignature", &compat.allow_empty_signature),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsTemperature", &compat.supports_temperature),
             }) {
            auto parsed = optional_scalar_bool(**object, key, std::format("{}.compat", path));
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if ((*parsed).has_value()) {
                *target = **parsed;
                populated = true;
            }
        }
        if (populated) {
            return std::optional<ai::ModelCompatVariant>{ai::ModelCompatVariant{std::move(compat)}};
        }
        return std::nullopt;
    }

    if (api == "openai-completions") {
        ai::OpenAICompletionsCompat compat;
        bool populated = false;
        for (const auto& [key, target] : std::array{
                     std::pair<std::string_view, std::optional<bool>*>("supportsStore", &compat.supports_store),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsDeveloperRole", &compat.supports_developer_role),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsStrictMode", &compat.supports_strict_mode),
                     std::pair<std::string_view, std::optional<bool>*>("requiresReasoningContentOnAssistantMessages",
                             &compat.requires_reasoning_content_on_assistant_messages),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsLongCacheRetention", &compat.supports_long_cache_retention),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsReasoningEffort", &compat.supports_reasoning_effort),
             }) {
            auto parsed = optional_scalar_bool(**object, key, std::format("{}.compat", path));
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if ((*parsed).has_value()) {
                *target = **parsed;
                populated = true;
            }
        }

        auto max_tokens = optional_scalar_string(**object, "maxTokensField", std::format("{}.compat", path));
        if (!max_tokens) {
            return std::unexpected(max_tokens.error());
        }
        if ((*max_tokens).has_value()) {
            if (**max_tokens == "max_tokens") {
                compat.max_tokens_field = ai::OpenAICompletionsMaxTokensField::MaxTokens;
            } else if (**max_tokens == "max_completion_tokens") {
                compat.max_tokens_field = ai::OpenAICompletionsMaxTokensField::MaxCompletionTokens;
            } else {
                return fail<std::optional<ai::ModelCompatVariant>>(
                        std::format("{}.compat.maxTokensField", path), "unknown max token field");
            }
            populated = true;
        }

        auto thinking_format = optional_scalar_string(**object, "thinkingFormat", std::format("{}.compat", path));
        if (!thinking_format) {
            return std::unexpected(thinking_format.error());
        }
        if ((*thinking_format).has_value()) {
            if (**thinking_format == "openai") {
                compat.thinking_format = ai::OpenAICompletionsThinkingFormat::OpenAI;
            } else if (**thinking_format == "openrouter") {
                compat.thinking_format = ai::OpenAICompletionsThinkingFormat::OpenRouter;
            } else if (**thinking_format == "deepseek") {
                compat.thinking_format = ai::OpenAICompletionsThinkingFormat::DeepSeek;
            } else if (**thinking_format == "qwen") {
                compat.thinking_format = ai::OpenAICompletionsThinkingFormat::Qwen;
            } else {
                return fail<std::optional<ai::ModelCompatVariant>>(
                        std::format("{}.compat.thinkingFormat", path), "unknown thinking format");
            }
            populated = true;
        }

        auto cache_format = optional_scalar_string(**object, "cacheControlFormat", std::format("{}.compat", path));
        if (!cache_format) {
            return std::unexpected(cache_format.error());
        }
        if ((*cache_format).has_value()) {
            if (**cache_format != "anthropic") {
                return fail<std::optional<ai::ModelCompatVariant>>(
                        std::format("{}.compat.cacheControlFormat", path), "unknown cache-control format");
            }
            compat.cache_control_format = ai::OpenAICompletionsCacheControlFormat::Anthropic;
            populated = true;
        }
        if (populated) {
            return std::optional<ai::ModelCompatVariant>{ai::ModelCompatVariant{std::move(compat)}};
        }
        return std::nullopt;
    }

    if (api == "openai-responses") {
        ai::OpenAIResponsesCompat compat;
        bool populated = false;
        auto session_affinity =
                optional_scalar_string(**object, "sessionAffinityFormat", std::format("{}.compat", path));
        if (!session_affinity) {
            return std::unexpected(session_affinity.error());
        }
        if ((*session_affinity).has_value()) {
            if (**session_affinity == "openai") {
                compat.session_affinity_format = ai::OpenAIResponsesSessionAffinityFormat::OpenAI;
            } else if (**session_affinity == "openai-nosession") {
                compat.session_affinity_format = ai::OpenAIResponsesSessionAffinityFormat::OpenAINoSession;
            } else if (**session_affinity == "openrouter") {
                compat.session_affinity_format = ai::OpenAIResponsesSessionAffinityFormat::OpenRouter;
            } else {
                return fail<std::optional<ai::ModelCompatVariant>>(
                        std::format("{}.compat.sessionAffinityFormat", path), "unknown session-affinity format");
            }
            populated = true;
        }
        for (const auto& [key, target] : std::array{
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsStrictMode", &compat.supports_strict_mode),
                     std::pair<std::string_view, std::optional<bool>*>(
                             "supportsExplicitPromptCacheMode", &compat.supports_explicit_prompt_cache_mode),
             }) {
            auto parsed = optional_scalar_bool(**object, key, std::format("{}.compat", path));
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if ((*parsed).has_value()) {
                *target = **parsed;
                populated = true;
            }
        }
        if (populated) {
            return std::optional<ai::ModelCompatVariant>{ai::ModelCompatVariant{std::move(compat)}};
        }
    }
    return std::nullopt;
}

[[nodiscard]] ParseResult<ai::Model> parse_model(std::string_view api, const Object& object, std::string path) {
    auto id = required_string_member(object, "id", path);
    auto name = required_string_member(object, "name", path);
    auto model_api = required_string_member(object, "api", path);
    auto provider = required_string_member(object, "provider", path);
    auto base_url = required_string_member(object, "baseUrl", path);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!model_api) {
        return std::unexpected(model_api.error());
    }
    if (!provider) {
        return std::unexpected(provider.error());
    }
    if (!base_url) {
        return std::unexpected(base_url.error());
    }

    auto reasoning_value = required_member(object, "reasoning", path);
    auto input_value = required_member(object, "input", path);
    auto cost_value = required_member(object, "cost", path);
    auto context_value = required_member(object, "contextWindow", path);
    auto max_tokens_value = required_member(object, "maxTokens", path);
    if (!reasoning_value) {
        return std::unexpected(reasoning_value.error());
    }
    if (!input_value) {
        return std::unexpected(input_value.error());
    }
    if (!cost_value) {
        return std::unexpected(cost_value.error());
    }
    if (!context_value) {
        return std::unexpected(context_value.error());
    }
    if (!max_tokens_value) {
        return std::unexpected(max_tokens_value.error());
    }
    auto reasoning = required_bool(**reasoning_value, std::format("{}.reasoning", path));
    auto input_array = required_array(**input_value, std::format("{}.input", path));
    auto cost = parse_cost(**cost_value, std::format("{}.cost", path));
    auto context = required_uint64(**context_value, std::format("{}.contextWindow", path));
    auto max_tokens = required_uint64(**max_tokens_value, std::format("{}.maxTokens", path));
    if (!reasoning) {
        return std::unexpected(reasoning.error());
    }
    if (!input_array) {
        return std::unexpected(input_array.error());
    }
    if (!cost) {
        return std::unexpected(cost.error());
    }
    if (!context) {
        return std::unexpected(context.error());
    }
    if (!max_tokens) {
        return std::unexpected(max_tokens.error());
    }

    std::vector<ai::ModelInput> input;
    input.reserve((*input_array)->size());
    for (std::size_t index = 0; index < (*input_array)->size(); ++index) {
        auto modality = required_string((*input_array)->at(index), std::format("{}.input[{}]", path, index));
        if (!modality) {
            return std::unexpected(modality.error());
        }
        auto parsed = parse_input_modality(**modality, std::format("{}.input[{}]", path, index));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        input.push_back(*parsed);
    }

    auto thinking_map = parse_thinking_map(object, path);
    auto headers = parse_headers(object, path);
    auto compat = parse_compat(api, object, path);
    if (!thinking_map) {
        return std::unexpected(thinking_map.error());
    }
    if (!headers) {
        return std::unexpected(headers.error());
    }
    if (!compat) {
        return std::unexpected(compat.error());
    }

    ai::Model model;
    model.id = **id;
    model.name = **name;
    model.api = **model_api;
    model.provider = **provider;
    model.base_url = **base_url;
    model.reasoning = *reasoning;
    model.input = std::move(input);
    model.cost = std::move(*cost);
    model.context_window = *context;
    model.max_tokens = *max_tokens;
    model.thinking_level_map = std::move(*thinking_map);
    model.headers = std::move(*headers);
    model.compat = std::move(*compat);
    return model;
}

struct ExpectedCatalog {
    std::map<std::string, ai::Model> models;
    std::map<std::string, std::vector<std::string>> model_ids_by_api;
};

[[nodiscard]] ParseResult<ExpectedCatalog> parse_catalog(std::string_view provider_id, const JsonValue& raw) {
    auto root = required_object(raw, std::string{provider_id});
    if (!root) {
        return std::unexpected(root.error());
    }
    if ((*root)->empty()) {
        return fail<ExpectedCatalog>(std::string{provider_id}, "catalog is empty");
    }

    ExpectedCatalog catalog;
    for (const auto& [api, models_value] : **root) {
        if (api.empty()) {
            return fail<ExpectedCatalog>(std::string{provider_id}, "empty outer API group");
        }
        auto models = required_object(models_value, std::format("{}/{}", provider_id, api));
        if (!models) {
            return std::unexpected(models.error());
        }
        if ((*models)->empty()) {
            return fail<ExpectedCatalog>(std::format("{}/{}", provider_id, api), "API group is empty");
        }
        auto& ids = catalog.model_ids_by_api[api];
        for (const auto& [model_id, model_value] : **models) {
            const auto path = std::format("{}/{}/{}", provider_id, api, model_id);
            auto model_object = required_object(model_value, path);
            if (!model_object) {
                return std::unexpected(model_object.error());
            }
            auto raw_id = required_string_member(**model_object, "id", path);
            auto raw_api = required_string_member(**model_object, "api", path);
            auto raw_provider = required_string_member(**model_object, "provider", path);
            if (!raw_id) {
                return std::unexpected(raw_id.error());
            }
            if (!raw_api) {
                return std::unexpected(raw_api.error());
            }
            if (!raw_provider) {
                return std::unexpected(raw_provider.error());
            }
            if (**raw_id != model_id) {
                return fail<ExpectedCatalog>(path + ".id", "model key disagrees with id");
            }
            if (**raw_api != api) {
                return fail<ExpectedCatalog>(path + ".api", "model api disagrees with the outer API group");
            }
            if (**raw_provider != provider_id) {
                return fail<ExpectedCatalog>(path + ".provider", "model provider disagrees with the artifact provider");
            }

            auto audited = audit_model_fields(api, **model_object, path);
            if (!audited) {
                return std::unexpected(audited.error());
            }
            auto model = parse_model(api, **model_object, path);
            if (!model) {
                return std::unexpected(model.error());
            }
            if (!catalog.models.emplace(model_id, std::move(*model)).second) {
                return fail<ExpectedCatalog>(path, "duplicate model id across API groups");
            }
            ids.push_back(model_id);
        }
    }
    return catalog;
}

[[nodiscard]] ParseResult<ExpectedCatalog> load_catalog_fixture(
        std::string_view provider_id, std::string_view fixture_path) {
    auto raw = tests::read_pi_fixture(fixture_path);
    if (!raw) {
        return fail<ExpectedCatalog>(std::string{fixture_path}, raw.error().message);
    }
    return parse_catalog(provider_id, *raw);
}

[[nodiscard]] ParseResult<ExpectedCatalog> load_catalog(std::string_view provider_id) {
    const auto path = std::string{"models/providers/"} + std::string{provider_id} + ".json";
    return load_catalog_fixture(provider_id, path);
}

[[nodiscard]] ParseResult<std::vector<std::string>> parse_string_array(const JsonValue& value, std::string path) {
    auto array = required_array(value, path);
    if (!array) {
        return std::unexpected(array.error());
    }
    std::vector<std::string> result;
    result.reserve((*array)->size());
    for (std::size_t index = 0; index < (*array)->size(); ++index) {
        auto text = required_string((*array)->at(index), std::format("{}[{}]", path, index));
        if (!text) {
            return std::unexpected(text.error());
        }
        result.emplace_back(**text);
    }
    return result;
}

[[nodiscard]] ParseResult<std::uint64_t> required_uint64_member(
        const Object& object, std::string_view key, std::string_view path) {
    auto value = required_member(object, key, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return required_uint64(**value, std::format("{}.{}", path, key));
}

[[nodiscard]] ParseResult<void> verify_provenance_snapshot() {
    auto provenance = tests::read_pi_fixture("models/provenance.json");
    if (!provenance) {
        return fail<void>("models/provenance.json", provenance.error().message);
    }
    auto root = required_object(*provenance, "models/provenance.json");
    if (!root) {
        return std::unexpected(root.error());
    }
    auto schema = required_string_member(**root, "schema", "models/provenance.json");
    if (!schema) {
        return std::unexpected(schema.error());
    }
    if (**schema != "cpp-coding-harness/pi-ai-provenance/1") {
        return fail<void>("models/provenance.json.schema", "unexpected schema");
    }
    auto source_value = required_member(**root, "source", "models/provenance.json");
    auto providers_value = required_member(**root, "providers", "models/provenance.json");
    if (!source_value) {
        return std::unexpected(source_value.error());
    }
    if (!providers_value) {
        return std::unexpected(providers_value.error());
    }
    auto source = required_object(**source_value, "models/provenance.json.source");
    auto providers = required_object(**providers_value, "models/provenance.json.providers");
    if (!source) {
        return std::unexpected(source.error());
    }
    if (!providers) {
        return std::unexpected(providers.error());
    }
    auto revision = required_string_member(**source, "revision", "models/provenance.json.source");
    auto command = required_string_member(**source, "command", "models/provenance.json.source");
    auto generated_at = required_string_member(**source, "generated_at", "models/provenance.json.source");
    if (!revision) {
        return std::unexpected(revision.error());
    }
    if (!command) {
        return std::unexpected(command.error());
    }
    if (!generated_at) {
        return std::unexpected(generated_at.error());
    }
    if (**revision != "1a584a7a56eb5e7b4ff8ccbd46430f1533282eed") {
        return fail<void>("models/provenance.json.source.revision", "wrong pinned revision");
    }
    if (**command != "node packages/ai/scripts/generate-models.ts --strict") {
        return fail<void>("models/provenance.json.source.command", "wrong generator command");
    }
    if ((**generated_at).empty()) {
        return fail<void>("models/provenance.json.source.generated_at", "missing generation timestamp");
    }

    std::set<std::string> recorded_providers;
    for (const auto& [provider, _] : **providers) {
        recorded_providers.insert(provider);
    }
    std::set<std::string> expected_providers;
    for (const auto provider : kUpstreamProviders) {
        expected_providers.emplace(provider);
    }
    if (recorded_providers != expected_providers) {
        return fail<void>("models/provenance.json.providers", "provider set differs from T0");
    }

    for (const auto provider : kUpstreamProviders) {
        const auto provider_path = std::format("models/provenance.json.providers.{}", provider);
        const auto* record_value = member(**providers, provider);
        if (record_value == nullptr) {
            return fail<void>(provider_path, "missing provider record");
        }
        auto record = required_object(*record_value, provider_path);
        if (!record) {
            return std::unexpected(record.error());
        }
        const auto expected_path = std::format("models/providers/{}.json", provider);
        auto path = required_string_member(**record, "path", provider_path);
        auto sha = required_string_member(**record, "sha256", provider_path);
        if (!path) {
            return std::unexpected(path.error());
        }
        if (!sha) {
            return std::unexpected(sha.error());
        }
        if (**path != expected_path) {
            return fail<void>(provider_path + ".path", "artifact path differs from provider id");
        }
        if ((**sha).size() != 64) {
            return fail<void>(provider_path + ".sha256", "hash is not a SHA-256 hex string");
        }

        auto artifact_text = tests::read_pi_fixture_text(**path);
        auto catalog = load_catalog(provider);
        if (!artifact_text) {
            return fail<void>(**path, artifact_text.error().message);
        }
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        auto bytes = required_uint64_member(**record, "bytes", provider_path);
        auto model_count = required_uint64_member(**record, "model_count", provider_path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (!model_count) {
            return std::unexpected(model_count.error());
        }
        if (*bytes != artifact_text->size()) {
            return fail<void>(provider_path + ".bytes", "artifact byte count differs");
        }
        if (*model_count != catalog->models.size()) {
            return fail<void>(provider_path + ".model_count", "artifact model count differs");
        }

        auto recorded_ids_value = required_member(**record, "model_ids", provider_path);
        auto recorded_apis_value = required_member(**record, "apis", provider_path);
        if (!recorded_ids_value) {
            return std::unexpected(recorded_ids_value.error());
        }
        if (!recorded_apis_value) {
            return std::unexpected(recorded_apis_value.error());
        }
        auto recorded_ids = parse_string_array(**recorded_ids_value, provider_path + ".model_ids");
        if (!recorded_ids) {
            return std::unexpected(recorded_ids.error());
        }
        std::vector<std::string> actual_ids;
        actual_ids.reserve(catalog->models.size());
        for (const auto& [model_id, _] : catalog->models) {
            actual_ids.push_back(model_id);
        }
        if (*recorded_ids != actual_ids) {
            return fail<void>(provider_path + ".model_ids", "artifact model set differs");
        }

        auto recorded_apis = required_object(**recorded_apis_value, provider_path + ".apis");
        if (!recorded_apis) {
            return std::unexpected(recorded_apis.error());
        }
        std::map<std::string, std::vector<std::string>> actual_apis = catalog->model_ids_by_api;
        if ((*recorded_apis)->size() != actual_apis.size()) {
            return fail<void>(provider_path + ".apis", "API grouping differs");
        }
        for (const auto& [api, ids] : actual_apis) {
            const auto* recorded_api = member(**recorded_apis, api);
            if (recorded_api == nullptr) {
                return fail<void>(provider_path + ".apis." + api, "missing API group");
            }
            auto expected_api_ids = parse_string_array(*recorded_api, provider_path + ".apis." + api);
            if (!expected_api_ids) {
                return std::unexpected(expected_api_ids.error());
            }
            if (*expected_api_ids != ids) {
                return fail<void>(provider_path + ".apis." + api, "model IDs differ");
            }
        }
    }
    return {};
}

[[nodiscard]] const ai::ProviderDefinition* find_provider(
        const std::vector<ai::ProviderDefinition>& definitions, std::string_view provider_id) {
    const auto found = std::ranges::find(
            definitions, provider_id, [](const auto& definition) -> std::string_view { return definition.id; });
    return found == definitions.end() ? nullptr : &*found;
}

[[nodiscard]] std::string_view input_name(ai::ModelInput input) {
    switch (input) {
    case ai::ModelInput::Text:
        return "text";
    case ai::ModelInput::Image:
        return "image";
    }
    return "unknown";
}

[[nodiscard]] std::string thinking_level_path(ai::ModelThinkingLevel level) {
    if (const auto name = ai::model_thinking_level_name(level)) {
        return std::string{*name};
    }
    return std::format("level-{}", static_cast<int>(level));
}

void add_mismatch(std::vector<std::string>& mismatches, std::string path) { mismatches.push_back(std::move(path)); }

template <typename T>
void compare_optional(const std::optional<T>& expected,
        const std::optional<T>& actual,
        std::string path,
        std::vector<std::string>& mismatches) {
    if (expected.has_value() != actual.has_value()) {
        add_mismatch(mismatches, std::move(path));
    } else if (expected.has_value() && *expected != *actual) {
        add_mismatch(mismatches, std::move(path));
    }
}

void compare_thinking_maps(const std::optional<ai::ThinkingLevelMap>& expected,
        const std::optional<ai::ThinkingLevelMap>& actual,
        std::string path,
        std::vector<std::string>& mismatches) {
    if (expected.has_value() != actual.has_value()) {
        add_mismatch(mismatches, std::move(path));
        return;
    }
    if (!expected) {
        return;
    }
    for (const auto& [level, expected_value] : *expected) {
        const auto found = actual->find(level);
        const auto item_path = path + "." + thinking_level_path(level);
        if (found == actual->end()) {
            add_mismatch(mismatches, item_path);
        } else if (found->second != expected_value) {
            add_mismatch(mismatches, item_path);
        }
    }
    for (const auto& [level, _] : *actual) {
        if (!expected->contains(level)) {
            add_mismatch(mismatches, path + "." + thinking_level_path(level));
        }
    }
}

void compare_costs(const ai::ModelCost& expected,
        const ai::ModelCost& actual,
        std::string path,
        std::vector<std::string>& mismatches) {
    if (expected.input != actual.input) {
        add_mismatch(mismatches, path + ".input");
    }
    if (expected.output != actual.output) {
        add_mismatch(mismatches, path + ".output");
    }
    if (expected.cache_read != actual.cache_read) {
        add_mismatch(mismatches, path + ".cacheRead");
    }
    if (expected.cache_write != actual.cache_write) {
        add_mismatch(mismatches, path + ".cacheWrite");
    }
    if (expected.tiers.has_value() != actual.tiers.has_value()) {
        add_mismatch(mismatches, path + ".tiers");
        return;
    }
    if (!expected.tiers) {
        return;
    }
    if (expected.tiers->size() != actual.tiers->size()) {
        add_mismatch(mismatches, path + ".tiers");
        return;
    }
    for (std::size_t index = 0; index < expected.tiers->size(); ++index) {
        const auto tier_path = std::format("{}.tiers[{}]", path, index);
        const auto& expected_tier = expected.tiers->at(index);
        const auto& actual_tier = actual.tiers->at(index);
        if (expected_tier.input != actual_tier.input) {
            add_mismatch(mismatches, tier_path + ".input");
        }
        if (expected_tier.output != actual_tier.output) {
            add_mismatch(mismatches, tier_path + ".output");
        }
        if (expected_tier.cache_read != actual_tier.cache_read) {
            add_mismatch(mismatches, tier_path + ".cacheRead");
        }
        if (expected_tier.cache_write != actual_tier.cache_write) {
            add_mismatch(mismatches, tier_path + ".cacheWrite");
        }
        if (expected_tier.input_tokens_above != actual_tier.input_tokens_above) {
            add_mismatch(mismatches, tier_path + ".inputTokensAbove");
        }
    }
}

void compare_headers(const std::optional<ai::ModelHeaders>& expected,
        const std::optional<ai::ModelHeaders>& actual,
        std::string path,
        std::vector<std::string>& mismatches) {
    if (expected.has_value() != actual.has_value()) {
        add_mismatch(mismatches, std::move(path));
        return;
    }
    if (!expected) {
        return;
    }
    for (const auto& [key, expected_value] : *expected) {
        const auto found = actual->find(key);
        if (found == actual->end()) {
            add_mismatch(mismatches, path + "." + key);
        } else if (found->second != expected_value) {
            add_mismatch(mismatches, path + "." + key);
        }
    }
    for (const auto& [key, _] : *actual) {
        if (!expected->contains(key)) {
            add_mismatch(mismatches, path + "." + key);
        }
    }
}

void compare_compat(const std::optional<ai::ModelCompatVariant>& expected,
        const std::optional<ai::ModelCompatVariant>& actual,
        std::vector<std::string>& mismatches) {
    if (expected.has_value() != actual.has_value()) {
        add_mismatch(mismatches, "compat");
        return;
    }
    if (!expected) {
        return;
    }
    if (expected->index() != actual->index()) {
        add_mismatch(mismatches, "compat.alternative");
        return;
    }
    std::visit(
            [&](const auto& expected_value, const auto& actual_value) {
                using Expected = std::decay_t<decltype(expected_value)>;
                using Actual = std::decay_t<decltype(actual_value)>;
                if constexpr (std::is_same_v<Expected, Actual>) {
                    if constexpr (std::is_same_v<Expected, ai::AnthropicMessagesCompat>) {
                        compare_optional(expected_value.force_adaptive_thinking,
                                actual_value.force_adaptive_thinking,
                                "compat.forceAdaptiveThinking",
                                mismatches);
                        compare_optional(expected_value.allow_empty_signature,
                                actual_value.allow_empty_signature,
                                "compat.allowEmptySignature",
                                mismatches);
                        compare_optional(expected_value.supports_temperature,
                                actual_value.supports_temperature,
                                "compat.supportsTemperature",
                                mismatches);
                    } else if constexpr (std::is_same_v<Expected, ai::OpenAICompletionsCompat>) {
                        compare_optional(expected_value.supports_store,
                                actual_value.supports_store,
                                "compat.supportsStore",
                                mismatches);
                        compare_optional(expected_value.supports_developer_role,
                                actual_value.supports_developer_role,
                                "compat.supportsDeveloperRole",
                                mismatches);
                        compare_optional(expected_value.supports_strict_mode,
                                actual_value.supports_strict_mode,
                                "compat.supportsStrictMode",
                                mismatches);
                        compare_optional(expected_value.max_tokens_field,
                                actual_value.max_tokens_field,
                                "compat.maxTokensField",
                                mismatches);
                        compare_optional(expected_value.requires_reasoning_content_on_assistant_messages,
                                actual_value.requires_reasoning_content_on_assistant_messages,
                                "compat.requiresReasoningContentOnAssistantMessages",
                                mismatches);
                        compare_optional(expected_value.thinking_format,
                                actual_value.thinking_format,
                                "compat.thinkingFormat",
                                mismatches);
                        compare_optional(expected_value.cache_control_format,
                                actual_value.cache_control_format,
                                "compat.cacheControlFormat",
                                mismatches);
                        compare_optional(expected_value.supports_long_cache_retention,
                                actual_value.supports_long_cache_retention,
                                "compat.supportsLongCacheRetention",
                                mismatches);
                        compare_optional(expected_value.supports_reasoning_effort,
                                actual_value.supports_reasoning_effort,
                                "compat.supportsReasoningEffort",
                                mismatches);
                    } else {
                        compare_optional(expected_value.session_affinity_format,
                                actual_value.session_affinity_format,
                                "compat.sessionAffinityFormat",
                                mismatches);
                        compare_optional(expected_value.supports_strict_mode,
                                actual_value.supports_strict_mode,
                                "compat.supportsStrictMode",
                                mismatches);
                        compare_optional(expected_value.supports_explicit_prompt_cache_mode,
                                actual_value.supports_explicit_prompt_cache_mode,
                                "compat.supportsExplicitPromptCacheMode",
                                mismatches);
                    }
                }
            },
            *expected,
            *actual);
}

[[nodiscard]] std::vector<std::string> compare_models(const ai::Model& expected, const ai::Model& actual) {
    std::vector<std::string> mismatches;
    if (expected.id != actual.id) {
        add_mismatch(mismatches, "id");
    }
    if (expected.name != actual.name) {
        add_mismatch(mismatches, "name");
    }
    if (expected.api != actual.api) {
        add_mismatch(mismatches, "api");
    }
    if (expected.provider != actual.provider) {
        add_mismatch(mismatches, "provider");
    }
    if (expected.base_url != actual.base_url) {
        add_mismatch(mismatches, "baseUrl");
    }
    if (expected.reasoning != actual.reasoning) {
        add_mismatch(mismatches, "reasoning");
    }
    compare_thinking_maps(expected.thinking_level_map, actual.thinking_level_map, "thinkingLevelMap", mismatches);
    if (expected.input.size() != actual.input.size()) {
        add_mismatch(mismatches, "input");
    } else {
        for (std::size_t index = 0; index < expected.input.size(); ++index) {
            if (expected.input[index] != actual.input[index]) {
                add_mismatch(mismatches, std::format("input[{}] ({})", index, input_name(expected.input[index])));
            }
        }
    }
    compare_costs(expected.cost, actual.cost, "cost", mismatches);
    if (expected.context_window != actual.context_window) {
        add_mismatch(mismatches, "contextWindow");
    }
    if (expected.max_tokens != actual.max_tokens) {
        add_mismatch(mismatches, "maxTokens");
    }
    compare_headers(expected.headers, actual.headers, "headers", mismatches);
    compare_compat(expected.compat, actual.compat, mismatches);
    return mismatches;
}

[[nodiscard]] bool has_mismatch_path(const std::vector<std::string>& mismatches, std::string_view path) {
    return std::ranges::any_of(mismatches, [&](const auto& mismatch) { return mismatch.starts_with(path); });
}

[[nodiscard]] std::string mismatch_summary(const std::vector<std::string>& mismatches) {
    std::string summary;
    for (const auto& mismatch : mismatches) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += mismatch;
    }
    return summary;
}

[[nodiscard]] const ai::Model* find_model(const ai::ProviderDefinition& definition, std::string_view model_id) {
    const auto found = std::ranges::find(
            definition.models, model_id, [](const auto& model) -> std::string_view { return model.id; });
    return found == definition.models.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("the six raw provider artifacts agree with their provenance model sets and API topology",
        "[ai][catalog][issue765][compat-pi]") {
    const auto result = verify_provenance_snapshot();
    INFO((result ? "" : result.error()));
    REQUIRE(result);
}

TEST_CASE("the built-in catalog matches every carried raw vendored field", "[ai][catalog][issue765][compat-pi]") {
    const auto definitions = ai::builtin_provider_definitions();
    REQUIRE(definitions.size() == kUpstreamProviders.size());

    std::set<std::string> actual_provider_ids;
    for (const auto& definition : definitions) {
        actual_provider_ids.insert(definition.id);
    }
    std::set<std::string> expected_provider_ids;
    for (const auto provider : kUpstreamProviders) {
        expected_provider_ids.emplace(provider);
    }
    CHECK(actual_provider_ids == expected_provider_ids);

    for (const auto provider : kMirrorProviders) {
        auto expected = load_catalog(provider);
        INFO((expected ? "" : expected.error()));
        REQUIRE(expected);
        const auto* actual_provider = find_provider(definitions, provider);
        REQUIRE(actual_provider != nullptr);
        CHECK(actual_provider->models.size() == expected->models.size());

        std::map<std::string, const ai::Model*> actual_models;
        for (const auto& model : actual_provider->models) {
            const auto [_, inserted] = actual_models.emplace(model.id, &model);
            INFO("duplicate actual model: " + model.id);
            CHECK(inserted);
        }
        CHECK(actual_models.size() == actual_provider->models.size());

        for (const auto& [model_id, expected_model] : expected->models) {
            const auto actual = actual_models.find(model_id);
            INFO(std::string{provider} + "/" + model_id);
            REQUIRE(actual != actual_models.end());

            const auto valid = ai::validate_model(*actual->second);
            INFO((valid ? "" : valid.error().detail));
            CHECK(valid);

            const auto mismatches = compare_models(expected_model, *actual->second);
            INFO(mismatch_summary(mismatches));
            CHECK(mismatches.empty());
        }
        for (const auto& [model_id, _] : actual_models) {
            if (!expected->models.contains(model_id)) {
                INFO(std::string{provider} + "/" + model_id);
                CHECK(false);
            }
        }
    }

    // Kimi's final model values are the vendor oracle owned by #763. T7 only
    // keeps its generated model count in the six-provider catalog set.
    const auto* kimi = find_provider(definitions, "kimi-coding");
    REQUIRE(kimi != nullptr);
    CHECK(kimi->models.size() == 4);
}

TEST_CASE("the built-in Kimi catalog matches every vendor-pinned field", "[ai][catalog][issue763][compat-vendor]") {
    const auto expected = load_catalog_fixture("kimi-coding", "models/vendors/kimi-coding.json");
    INFO((expected ? "" : expected.error()));
    REQUIRE(expected);

    const auto definitions = ai::builtin_provider_definitions();
    const auto* actual_provider = find_provider(definitions, "kimi-coding");
    REQUIRE(actual_provider != nullptr);
    REQUIRE(actual_provider->models.size() == expected->models.size());

    std::map<std::string, const ai::Model*> actual_models;
    for (const auto& model : actual_provider->models) {
        actual_models.emplace(model.id, &model);
    }
    for (const auto& [model_id, expected_model] : expected->models) {
        const auto actual = actual_models.find(model_id);
        INFO("kimi-coding/" + model_id);
        REQUIRE(actual != actual_models.end());
        const auto mismatches = compare_models(expected_model, *actual->second);
        INFO(mismatch_summary(mismatches));
        CHECK(mismatches.empty());
    }
}

TEST_CASE("raw catalog topology rejects wrong outer and model API identities", "[ai][catalog][issue765][compat-pi]") {
    auto raw = tests::read_pi_fixture("models/providers/deepseek.json");
    REQUIRE(raw);

    auto model_api_mutation = *raw;
    auto& model_api_root = model_api_mutation.get_object();
    auto model_api_group = model_api_root.find("openai-completions");
    REQUIRE(model_api_group != model_api_root.end());
    auto& model_api_models = model_api_group->second.get_object();
    auto model_api_entry = model_api_models.find("deepseek-flash");
    REQUIRE(model_api_entry != model_api_models.end());
    model_api_entry->second.get_object()["api"] = JsonValue{"openai-responses"};
    auto wrong_model_api = parse_catalog("deepseek", model_api_mutation);
    REQUIRE_FALSE(wrong_model_api);
    CHECK(wrong_model_api.error().find("outer API group") != std::string::npos);

    auto outer_api_mutation = *raw;
    auto& outer_root = outer_api_mutation.get_object();
    auto outer_group = outer_root.find("openai-completions");
    REQUIRE(outer_group != outer_root.end());
    auto moved_group = std::move(outer_group->second);
    outer_root.erase(outer_group);
    outer_root.emplace("openai-responses", std::move(moved_group));
    auto wrong_outer_api = parse_catalog("deepseek", outer_api_mutation);
    REQUIRE_FALSE(wrong_outer_api);
    CHECK(wrong_outer_api.error().find("outer API group") != std::string::npos);
}

TEST_CASE("typed compatibility alternatives reject wrong API identities", "[ai][catalog][issue765][compat-pi]") {
    auto expected_catalog = load_catalog("deepseek");
    REQUIRE(expected_catalog);
    const auto expected = expected_catalog->models.find("deepseek-flash");
    REQUIRE(expected != expected_catalog->models.end());

    auto wrong_api = expected->second;
    wrong_api.api = "openai-responses";
    const auto invalid_api = ai::validate_model(wrong_api);
    REQUIRE_FALSE(invalid_api);

    auto wrong_alternative = expected->second;
    wrong_alternative.compat = ai::ModelCompatVariant{ai::OpenAIResponsesCompat{.supports_strict_mode = true}};
    const auto invalid_alternative = ai::validate_model(wrong_alternative);
    REQUIRE_FALSE(invalid_alternative);

    const auto mismatches = compare_models(expected->second, wrong_alternative);
    CHECK(has_mismatch_path(mismatches, "compat"));
}

TEST_CASE("the comparator catches dropped nested cost and thinking fields", "[ai][catalog][issue765][compat-pi]") {
    auto codex_catalog = load_catalog("openai-codex");
    REQUIRE(codex_catalog);
    const auto cost_model = codex_catalog->models.find("gpt-5.5");
    REQUIRE(cost_model != codex_catalog->models.end());
    REQUIRE(cost_model->second.cost.tiers);

    auto dropped_cost = cost_model->second;
    dropped_cost.cost.tiers->front().output = 0;
    const auto cost_mismatches = compare_models(cost_model->second, dropped_cost);
    CHECK(has_mismatch_path(cost_mismatches, "cost.tiers[0].output"));

    const auto thinking_model = codex_catalog->models.find("gpt-6-astra");
    REQUIRE(thinking_model != codex_catalog->models.end());
    REQUIRE(thinking_model->second.thinking_level_map);
    auto dropped_thinking = thinking_model->second;
    dropped_thinking.thinking_level_map->erase(ai::ModelThinkingLevel::High);
    const auto thinking_mismatches = compare_models(thinking_model->second, dropped_thinking);
    CHECK(has_mismatch_path(thinking_mismatches, "thinkingLevelMap.high"));
}

TEST_CASE("the raw-field audit rejects an unknown nested upstream field", "[ai][catalog][issue765][compat-pi]") {
    auto raw = tests::read_pi_fixture("models/providers/deepseek.json");
    REQUIRE(raw);

    auto compat_mutation = *raw;
    auto& compat_root = compat_mutation.get_object();
    auto compat_group = compat_root.find("openai-completions");
    REQUIRE(compat_group != compat_root.end());
    auto& compat_model = compat_group->second.get_object().find("deepseek-flash")->second.get_object();
    compat_model.find("compat")->second.get_object().emplace("futureFlag", true);
    auto unknown_compat = parse_catalog("deepseek", compat_mutation);
    REQUIRE_FALSE(unknown_compat);
    CHECK(unknown_compat.error().find("compat.futureFlag") != std::string::npos);

    auto cost_mutation = *raw;
    auto& cost_root = cost_mutation.get_object();
    auto cost_group = cost_root.find("openai-completions");
    REQUIRE(cost_group != cost_root.end());
    auto& cost_model = cost_group->second.get_object().find("deepseek-flash")->second.get_object();
    cost_model.find("cost")->second.get_object().emplace("futureRate", 1.0);
    auto unknown_cost = parse_catalog("deepseek", cost_mutation);
    REQUIRE_FALSE(unknown_cost);
    CHECK(unknown_cost.error().find("cost.futureRate") != std::string::npos);
}

TEST_CASE("raw optional semantics preserve missing fields and explicit false values",
        "[ai][catalog][issue765][compat-pi]") {
    auto deepseek = load_catalog("deepseek");
    REQUIRE(deepseek);
    const auto flash = deepseek->models.find("deepseek-flash");
    REQUIRE(flash != deepseek->models.end());
    const auto* completions = std::get_if<ai::OpenAICompletionsCompat>(&*flash->second.compat);
    REQUIRE(completions != nullptr);
    CHECK(completions->supports_store == false);
    CHECK(completions->supports_developer_role == false);
    REQUIRE(flash->second.thinking_level_map);
    CHECK_FALSE(flash->second.thinking_level_map->contains(ai::ModelThinkingLevel::Off));
    CHECK(flash->second.thinking_level_map->at(ai::ModelThinkingLevel::Minimal) == std::nullopt);

    const auto deepseek_pro = deepseek->models.find("deepseek-v4-pro");
    REQUIRE(deepseek_pro != deepseek->models.end());
    auto dropped_false = flash->second;
    auto* dropped_compat = std::get_if<ai::OpenAICompletionsCompat>(&*dropped_false.compat);
    REQUIRE(dropped_compat != nullptr);
    dropped_compat->supports_store.reset();
    const auto mismatches = compare_models(flash->second, dropped_false);
    CHECK(has_mismatch_path(mismatches, "compat.supportsStore"));
    CHECK_FALSE(deepseek_pro->second.thinking_level_map->contains(ai::ModelThinkingLevel::Off));
}

TEST_CASE("OpenRouter retains provider-routed dynamic cost sentinels and generated counts",
        "[ai][catalog][issue765][compat-pi]") {
    auto expected = load_catalog("openrouter");
    REQUIRE(expected);
    CHECK(expected->models.size() == 378);
    const auto definitions = ai::builtin_provider_definitions();
    const auto* openrouter = find_provider(definitions, "openrouter");
    REQUIRE(openrouter != nullptr);
    CHECK(openrouter->models.size() == 378);

    for (const auto model_id : {"openrouter/auto", "openrouter/auto-beta"}) {
        const auto expected_model = expected->models.find(model_id);
        REQUIRE(expected_model != expected->models.end());
        CHECK(expected_model->second.cost.input == -1'000'000.0);
        CHECK(expected_model->second.cost.output == -1'000'000.0);
        CHECK(expected_model->second.cost.cache_read == 0.0);
        CHECK(expected_model->second.cost.cache_write == 0.0);

        const auto* actual_model = find_model(*openrouter, model_id);
        REQUIRE(actual_model != nullptr);
        CHECK(actual_model->cost.input == -1'000'000.0);
        CHECK(actual_model->cost.output == -1'000'000.0);
        CHECK(actual_model->cost.cache_read == 0.0);
        CHECK(actual_model->cost.cache_write == 0.0);
    }
}
