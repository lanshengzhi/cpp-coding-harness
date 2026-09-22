#include <cch/ai/Models.hpp>

#include "DefaultModelsJson.hpp"
#include "support/Json.hpp"
#include "ai/auth/KimiCodingOAuth.hpp"
#include "ai/auth/OpenAICodexOAuth.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai {
namespace {

[[nodiscard]] double number_or(const support::JsonValue::object_t& object, std::string_view key, double fallback) {
    const auto it = object.find(std::string{key});
    if (it == object.end()) {
        return fallback;
    }
    const auto* value = it->second.get_if<double>();
    return value == nullptr ? fallback : *value;
}

[[nodiscard]] std::vector<ModelCostTier> cost_tiers_from_catalog(const support::JsonValue::object_t& cost_obj) {
    std::vector<ModelCostTier> tiers;
    const auto it = cost_obj.find("tiers");
    if (it == cost_obj.end()) {
        return tiers;
    }
    const auto* array = it->second.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return tiers;
    }
    tiers.reserve(array->size());
    for (const auto& entry : *array) {
        const auto* tier_obj = entry.get_if<support::JsonValue::object_t>();
        if (tier_obj == nullptr) {
            continue;
        }
        ModelCostTier tier;
        tier.input = number_or(*tier_obj, "input", 0.0);
        tier.output = number_or(*tier_obj, "output", 0.0);
        tier.cache_read = number_or(*tier_obj, "cacheRead", 0.0);
        tier.cache_write = number_or(*tier_obj, "cacheWrite", 0.0);
        tier.input_tokens_above = static_cast<std::uint64_t>(number_or(*tier_obj, "inputTokensAbove", 0.0));
        tiers.push_back(std::move(tier));
    }
    return tiers;
}

[[nodiscard]] std::optional<bool> bool_member(const support::JsonValue::object_t& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        return std::nullopt;
    }
    if (const auto* value = found->second.get_if<bool>()) {
        return *value;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> string_member(
        const support::JsonValue::object_t& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        return std::nullopt;
    }
    if (const auto* value = found->second.get_if<std::string>()) {
        return *value;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ModelCompatVariant> model_compat_from_catalog(
        std::string_view api, const support::JsonValue::object_t& model_obj) {
    const auto found = model_obj.find("compat");
    if (found == model_obj.end()) {
        return std::nullopt;
    }
    const auto* compat_obj = found->second.get_if<support::JsonValue::object_t>();
    if (compat_obj == nullptr) {
        return std::nullopt;
    }

    if (api == "anthropic-messages") {
        AnthropicMessagesCompat compat;
        bool populated = false;
        if (const auto value = bool_member(*compat_obj, "forceAdaptiveThinking"); value.has_value()) {
            compat.force_adaptive_thinking = *value;
            populated = true;
        }
        if (const auto value = bool_member(*compat_obj, "allowEmptySignature"); value.has_value()) {
            compat.allow_empty_signature = *value;
            populated = true;
        }
        if (const auto value = bool_member(*compat_obj, "supportsTemperature"); value.has_value()) {
            compat.supports_temperature = *value;
            populated = true;
        }
        if (populated) {
            return ModelCompatVariant{std::move(compat)};
        }
        return std::nullopt;
    }

    if (api == "openai-completions") {
        OpenAICompletionsCompat compat;
        bool populated = false;
        if (const auto value = bool_member(*compat_obj, "supportsStore"); value.has_value()) {
            compat.supports_store = *value;
            populated = true;
        }
        if (const auto value = bool_member(*compat_obj, "supportsDeveloperRole"); value.has_value()) {
            compat.supports_developer_role = *value;
            populated = true;
        }
        if (const auto value = string_member(*compat_obj, "maxTokensField"); value.has_value()) {
            if (*value == "max_tokens") {
                compat.max_tokens_field = OpenAICompletionsMaxTokensField::MaxTokens;
                populated = true;
            } else if (*value == "max_completion_tokens") {
                compat.max_tokens_field = OpenAICompletionsMaxTokensField::MaxCompletionTokens;
                populated = true;
            }
        }
        if (const auto value = bool_member(*compat_obj, "requiresReasoningContentOnAssistantMessages");
                value.has_value()) {
            compat.requires_reasoning_content_on_assistant_messages = *value;
            populated = true;
        }
        if (const auto value = string_member(*compat_obj, "thinkingFormat"); value.has_value()) {
            if (*value == "openai") {
                compat.thinking_format = OpenAICompletionsThinkingFormat::OpenAI;
                populated = true;
            } else if (*value == "openrouter") {
                compat.thinking_format = OpenAICompletionsThinkingFormat::OpenRouter;
                populated = true;
            } else if (*value == "deepseek") {
                compat.thinking_format = OpenAICompletionsThinkingFormat::DeepSeek;
                populated = true;
            } else if (*value == "qwen") {
                compat.thinking_format = OpenAICompletionsThinkingFormat::Qwen;
                populated = true;
            }
        }
        if (const auto value = string_member(*compat_obj, "cacheControlFormat");
                value.has_value() && *value == "anthropic") {
            compat.cache_control_format = OpenAICompletionsCacheControlFormat::Anthropic;
            populated = true;
        }
        if (const auto value = bool_member(*compat_obj, "supportsLongCacheRetention"); value.has_value()) {
            compat.supports_long_cache_retention = *value;
            populated = true;
        }
        if (const auto value = bool_member(*compat_obj, "supportsReasoningEffort"); value.has_value()) {
            compat.supports_reasoning_effort = *value;
            populated = true;
        }
        if (populated) {
            return ModelCompatVariant{std::move(compat)};
        }
        return std::nullopt;
    }

    if (api == "openai-responses") {
        OpenAIResponsesCompat compat;
        bool populated = false;
        if (const auto value = bool_member(*compat_obj, "supportsStrictMode"); value.has_value()) {
            compat.supports_strict_mode = *value;
            populated = true;
        }
        if (const auto value = bool_member(*compat_obj, "supportsExplicitPromptCacheMode"); value.has_value()) {
            compat.supports_explicit_prompt_cache_mode = *value;
            populated = true;
        }
        // The pinned snapshot has sessionAffinityFormat only for OpenCode Go,
        // and no supportsMaxOutputTokens field. Those conflicting candidates
        // stay out of the value contract until a scoped consumer exists.
        if (populated) {
            return ModelCompatVariant{std::move(compat)};
        }
    }
    // Codex compat flags and other catalog-only fields are intentionally
    // ignored: the current Codex seam has no consumer for them.
    return std::nullopt;
}

[[nodiscard]] Model model_from_catalog(std::string_view provider_id,
        const support::JsonValue::object_t& model_obj,
        std::string_view default_api,
        std::string_view default_base_url) {
    Model model;
    if (const auto it = model_obj.find("id"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.id = *s;
        }
    }
    if (const auto it = model_obj.find("name"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.name = *s;
        }
    }
    if (model.name.empty()) {
        model.name = model.id;
    }
    model.provider = std::string{provider_id};

    if (const auto it = model_obj.find("api"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.api = *s;
        }
    }
    if (model.api.empty()) {
        model.api = std::string{default_api};
    }

    if (const auto it = model_obj.find("baseUrl"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.base_url = *s;
        }
    }
    if (model.base_url.empty()) {
        model.base_url = std::string{default_base_url};
    }

    if (const auto it = model_obj.find("reasoning"); it != model_obj.end()) {
        if (const auto* b = it->second.get_if<bool>()) {
            model.reasoning = *b;
        }
    }

    model.context_window = 128000;
    if (const auto it = model_obj.find("contextWindow"); it != model_obj.end()) {
        if (const auto* d = it->second.get_if<double>()) {
            model.context_window = static_cast<std::uint64_t>(*d);
        }
    }

    model.max_tokens = 16384;
    if (const auto it = model_obj.find("maxTokens"); it != model_obj.end()) {
        if (const auto* d = it->second.get_if<double>()) {
            model.max_tokens = static_cast<std::uint64_t>(*d);
        }
    }

    if (const auto it = model_obj.find("cost"); it != model_obj.end()) {
        if (const auto* cost_obj = it->second.get_if<support::JsonValue::object_t>()) {
            ModelCost cost;
            cost.input = number_or(*cost_obj, "input", 0.0);
            cost.output = number_or(*cost_obj, "output", 0.0);
            cost.cache_read = number_or(*cost_obj, "cacheRead", 0.0);
            cost.cache_write = number_or(*cost_obj, "cacheWrite", 0.0);
            auto tiers = cost_tiers_from_catalog(*cost_obj);
            if (!tiers.empty()) {
                cost.tiers = std::move(tiers);
            }
            model.cost = cost;
        }
    }

    if (const auto it = model_obj.find("headers"); it != model_obj.end()) {
        if (const auto* headers_obj = it->second.get_if<support::JsonValue::object_t>()) {
            ModelHeaders headers;
            for (const auto& [name, val] : *headers_obj) {
                if (const auto* s = val.get_if<std::string>()) {
                    headers.emplace(name, *s);
                }
            }
            if (!headers.empty()) {
                model.headers = std::move(headers);
            }
        }
    }

    if (const auto it = model_obj.find("thinkingLevelMap"); it != model_obj.end()) {
        if (const auto* map_obj = it->second.get_if<support::JsonValue::object_t>()) {
            ThinkingLevelMap map;
            for (const auto& [level_str, val] : *map_obj) {
                if (auto level = parse_model_thinking_level(level_str)) {
                    if (val.holds<support::JsonValue::null_t>()) {
                        // A JSON null marks the level explicitly unsupported.
                        map.emplace(*level, std::nullopt);
                        continue;
                    }
                    if (const auto* s = val.get_if<std::string>()) {
                        map.emplace(*level, *s);
                    }
                }
            }
            if (!map.empty()) {
                model.thinking_level_map = std::move(map);
            }
        }
    }

    if (provider_id == "kimi-coding") {
        model.input = {ModelInput::Text, ModelInput::Image};
    } else if (provider_id == "openai-codex" && model.id != "gpt-5.3-codex-spark") {
        model.input = {ModelInput::Text, ModelInput::Image};
    } else {
        model.input = {ModelInput::Text};
    }

    if (const auto compat = model_compat_from_catalog(model.api, model_obj); compat) {
        model.compat = *compat;
    } else if (provider_id == "kimi-coding") {
        model.compat = ModelCompatVariant{AnthropicMessagesCompat{
                .force_adaptive_thinking = true,
                .allow_empty_signature =
                        (model.id == "k3" || model.id == "kimi-for-coding") ? std::optional<bool>{true} : std::nullopt,
        }};
    }

    return model;
}

void bind_provider_auth(std::string_view provider_id, ProviderAuth& auth) {
    if (provider_id == "openai-codex") {
        auth.oauth = auth::make_openai_codex_oauth_auth();
    } else if (provider_id == "kimi-coding") {
        auth.oauth = auth::make_kimi_coding_oauth_auth();
        auto env_auth = providers::make_env_api_key_auth("Kimi API key", {"KIMI_API_KEY"});
        auth.api_key = std::move(*env_auth.api_key);
    } else if (provider_id == "deepseek") {
        auto env_auth = providers::make_env_api_key_auth("DeepSeek API key", {"DEEPSEEK_API_KEY"});
        auth.api_key = std::move(*env_auth.api_key);
    } else if (provider_id == "openrouter") {
        auto env_auth = providers::make_env_api_key_auth("OpenRouter API key", {"OPENROUTER_API_KEY"});
        auth.api_key = std::move(*env_auth.api_key);
    } else if (provider_id == "opencode-go") {
        auto env_auth = providers::make_env_api_key_auth("OpenCode API key", {"OPENCODE_API_KEY"});
        auth.api_key = std::move(*env_auth.api_key);
    } else if (provider_id == "openai") {
        auto env_auth = providers::make_env_api_key_auth("OpenAI API key", {"OPENAI_API_KEY"});
        auth.api_key = std::move(*env_auth.api_key);
    }
}

} // namespace

std::vector<ProviderDefinition> builtin_provider_definitions() {
    // debt: a malformed embedded catalog degrades to no built-in providers with
    // no diagnostic; the value-level catalog test in cch_tests_ai is the guard
    // that keeps this unreachable, so the signature stays infallible.
    auto parsed = support::read_json(default_models_json());
    if (!parsed) {
        return {};
    }
    const auto* root = parsed->get_if<support::JsonValue::object_t>();
    if (!root) {
        return {};
    }
    const auto prov_it = root->find("providers");
    if (prov_it == root->end()) {
        return {};
    }
    const auto* providers_obj = prov_it->second.get_if<support::JsonValue::object_t>();
    if (!providers_obj) {
        return {};
    }

    std::vector<ProviderDefinition> definitions;
    definitions.reserve(providers_obj->size());

    for (const auto& [provider_id, prov_val] : *providers_obj) {
        const auto* prov_data = prov_val.get_if<support::JsonValue::object_t>();
        if (!prov_data) continue;

        std::string name = provider_id;
        if (const auto it = prov_data->find("name"); it != prov_data->end()) {
            if (const auto* s = it->second.get_if<std::string>()) name = *s;
        }

        std::string base_url;
        if (const auto it = prov_data->find("baseUrl"); it != prov_data->end()) {
            if (const auto* s = it->second.get_if<std::string>()) base_url = *s;
        }

        std::string api;
        if (const auto it = prov_data->find("api"); it != prov_data->end()) {
            if (const auto* s = it->second.get_if<std::string>()) api = *s;
        }

        std::vector<Model> models;
        if (const auto it = prov_data->find("models"); it != prov_data->end()) {
            if (const auto* models_arr = it->second.get_if<support::JsonValue::array_t>()) {
                models.reserve(models_arr->size());
                for (const auto& m_val : *models_arr) {
                    if (const auto* m_obj = m_val.get_if<support::JsonValue::object_t>()) {
                        auto model = model_from_catalog(provider_id, *m_obj, api, base_url);
                        if (model.id.empty()) {
                            continue;
                        }
                        models.push_back(std::move(model));
                    }
                }
            }
        }

        ProviderAuth auth;
        bind_provider_auth(provider_id, auth);

        definitions.push_back(ProviderDefinition{
                .id = provider_id,
                .name = std::move(name),
                .models = std::move(models),
                .auth = std::move(auth),
        });
    }

    return definitions;
}

} // namespace cch::ai
