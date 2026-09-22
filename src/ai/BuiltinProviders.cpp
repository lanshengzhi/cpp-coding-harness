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

[[nodiscard]] std::optional<double> number_from_catalog(
        const support::JsonValue::object_t& object, std::string_view key) {
    const auto it = object.find(std::string{key});
    if (it == object.end()) {
        return std::nullopt;
    }
    const auto* value = it->second.get_if<double>();
    return value == nullptr ? std::nullopt : std::optional<double>{*value};
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
        const auto input = number_from_catalog(*tier_obj, "input");
        const auto output = number_from_catalog(*tier_obj, "output");
        const auto cache_read = number_from_catalog(*tier_obj, "cacheRead");
        const auto cache_write = number_from_catalog(*tier_obj, "cacheWrite");
        const auto input_tokens_above = number_from_catalog(*tier_obj, "inputTokensAbove");
        if (!input || !output || !cache_read || !cache_write || !input_tokens_above) {
            continue;
        }
        ModelCostTier tier;
        tier.input = *input;
        tier.output = *output;
        tier.cache_read = *cache_read;
        tier.cache_write = *cache_write;
        tier.input_tokens_above = static_cast<std::uint64_t>(*input_tokens_above);
        tiers.push_back(std::move(tier));
    }
    return tiers;
}

[[nodiscard]] Model model_from_catalog(const support::JsonValue::object_t& model_obj) {
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
    if (const auto it = model_obj.find("provider"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.provider = *s;
        }
    }

    if (const auto it = model_obj.find("api"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.api = *s;
        }
    }

    if (const auto it = model_obj.find("baseUrl"); it != model_obj.end()) {
        if (const auto* s = it->second.get_if<std::string>()) {
            model.base_url = *s;
        }
    }

    if (const auto it = model_obj.find("reasoning"); it != model_obj.end()) {
        if (const auto* b = it->second.get_if<bool>()) {
            model.reasoning = *b;
        }
    }

    if (const auto it = model_obj.find("contextWindow"); it != model_obj.end()) {
        if (const auto* d = it->second.get_if<double>()) {
            model.context_window = static_cast<std::uint64_t>(*d);
        }
    }

    if (const auto it = model_obj.find("maxTokens"); it != model_obj.end()) {
        if (const auto* d = it->second.get_if<double>()) {
            model.max_tokens = static_cast<std::uint64_t>(*d);
        }
    }

    if (const auto it = model_obj.find("cost"); it != model_obj.end()) {
        if (const auto* cost_obj = it->second.get_if<support::JsonValue::object_t>()) {
            ModelCost cost;
            if (const auto value = number_from_catalog(*cost_obj, "input")) {
                cost.input = *value;
            }
            if (const auto value = number_from_catalog(*cost_obj, "output")) {
                cost.output = *value;
            }
            if (const auto value = number_from_catalog(*cost_obj, "cacheRead")) {
                cost.cache_read = *value;
            }
            if (const auto value = number_from_catalog(*cost_obj, "cacheWrite")) {
                cost.cache_write = *value;
            }
            auto tiers = cost_tiers_from_catalog(*cost_obj);
            if (!tiers.empty()) {
                cost.tiers = std::move(tiers);
            }
            model.cost = cost;
        }
    }

    if (const auto it = model_obj.find("input"); it != model_obj.end()) {
        if (const auto* input_array = it->second.get_if<support::JsonValue::array_t>()) {
            model.input.reserve(input_array->size());
            for (const auto& input : *input_array) {
                const auto* input_name = input.get_if<std::string>();
                if (input_name == nullptr) {
                    continue;
                }
                if (*input_name == "text") {
                    model.input.push_back(ModelInput::Text);
                } else if (*input_name == "image") {
                    model.input.push_back(ModelInput::Image);
                }
            }
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

    if (const auto it = model_obj.find("compat"); it != model_obj.end() && model.api == "anthropic-messages") {
        if (const auto* compat_obj = it->second.get_if<support::JsonValue::object_t>()) {
            AnthropicMessagesCompat compat;
            if (const auto force = compat_obj->find("forceAdaptiveThinking"); force != compat_obj->end()) {
                if (const auto* value = force->second.get_if<bool>()) {
                    compat.force_adaptive_thinking = *value;
                }
            }
            if (const auto allow = compat_obj->find("allowEmptySignature"); allow != compat_obj->end()) {
                if (const auto* value = allow->second.get_if<bool>()) {
                    compat.allow_empty_signature = *value;
                }
            }
            if (compat.force_adaptive_thinking || compat.allow_empty_signature) {
                model.compat = std::move(compat);
            }
        }
    }

    return model;
}

[[nodiscard]] std::string provider_name(std::string_view provider_id) {
    if (provider_id == "deepseek") {
        return "DeepSeek";
    }
    if (provider_id == "openrouter") {
        return "OpenRouter";
    }
    if (provider_id == "opencode-go") {
        return "OpenCode Go";
    }
    if (provider_id == "openai") {
        return "OpenAI";
    }
    if (provider_id == "openai-codex") {
        return "OpenAI Codex";
    }
    if (provider_id == "kimi-coding") {
        return "Kimi For Coding";
    }
    return std::string{provider_id};
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

        std::vector<Model> models;
        if (const auto it = prov_data->find("models"); it != prov_data->end()) {
            if (const auto* models_arr = it->second.get_if<support::JsonValue::array_t>()) {
                models.reserve(models_arr->size());
                for (const auto& m_val : *models_arr) {
                    if (const auto* m_obj = m_val.get_if<support::JsonValue::object_t>()) {
                        auto model = model_from_catalog(*m_obj);
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
                .name = provider_name(provider_id),
                .models = std::move(models),
                .auth = std::move(auth),
        });
    }

    return definitions;
}

} // namespace cch::ai
