#include <cch/ai/Models.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <set>
#include <string_view>
#include <vector>

using namespace cch;

TEST_CASE("Built-in provider definitions preserve the frozen catalogs and auth methods",
        "[ai][providers][issue545][compat-pi]") {
    static_assert(std::movable<ai::ProviderDefinition>);
    static_assert(!std::copy_constructible<ai::ProviderDefinition>);

    auto definitions = ai::builtin_provider_definitions();

    REQUIRE(definitions.size() == 6);

    const auto codex_it =
            std::find_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.id == "openai-codex"; });
    REQUIRE(codex_it != definitions.end());
    const auto& codex = *codex_it;
    CHECK(codex.name == "OpenAI Codex");
    REQUIRE(codex.models.size() == 6);
    CHECK(codex.models.front().id == "gpt-5.3-codex-spark");
    CHECK(codex.models.back().id == "gpt-6-astra");
    REQUIRE(codex.auth.oauth);
    CHECK(codex.auth.oauth->name == "OpenAI (ChatGPT Plus/Pro)");
    CHECK(static_cast<bool>(codex.auth.oauth->login));
    CHECK(static_cast<bool>(codex.auth.oauth->refresh));
    CHECK(static_cast<bool>(codex.auth.oauth->to_auth));
    CHECK_FALSE(codex.auth.api_key);

    const auto kimi_it =
            std::find_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.id == "kimi-coding"; });
    REQUIRE(kimi_it != definitions.end());
    const auto& kimi = *kimi_it;
    CHECK(kimi.name == "Kimi For Coding");
    REQUIRE(kimi.models.size() == 4);
    CHECK(kimi.models.front().id == "k3");
    CHECK(kimi.models.back().id == "kimi-for-coding-highspeed");
    REQUIRE(kimi.auth.api_key);
    CHECK(kimi.auth.api_key->name == "Kimi API key");
    CHECK(static_cast<bool>(kimi.auth.api_key->check));
    CHECK(static_cast<bool>(kimi.auth.api_key->resolve));
    CHECK(static_cast<bool>(kimi.auth.api_key->login));
    REQUIRE(kimi.auth.oauth);
    CHECK(kimi.auth.oauth->name == "Kimi Code (subscription)");
    CHECK(static_cast<bool>(kimi.auth.oauth->login));
    CHECK(static_cast<bool>(kimi.auth.oauth->refresh));
    CHECK(static_cast<bool>(kimi.auth.oauth->to_auth));

    // Verify presence of all newly onboarded zero-config providers: each is
    // login-capable from `/login` without any user configuration.
    for (const auto& id : {"deepseek", "openrouter", "opencode-go", "openai"}) {
        const auto it = std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) { return d.id == id; });
        REQUIRE(it != definitions.end());
        REQUIRE(it->auth.api_key);
        CHECK(static_cast<bool>(it->auth.api_key->login));
        CHECK(static_cast<bool>(it->auth.api_key->resolve));
        CHECK_FALSE(it->models.empty());
    }

    const auto openrouter_it =
            std::find_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.id == "openrouter"; });
    REQUIRE(openrouter_it != definitions.end());
    REQUIRE(openrouter_it->auth.oauth);
    CHECK(openrouter_it->auth.oauth->name == "OpenRouter OAuth");
    CHECK(static_cast<bool>(openrouter_it->auth.oauth->login));
    CHECK(static_cast<bool>(openrouter_it->auth.oauth->refresh));
    CHECK(static_cast<bool>(openrouter_it->auth.oauth->to_auth));
}

TEST_CASE("Built-in provider definitions are fresh on every call", "[ai][providers][issue545][compat-pi]") {
    auto first = ai::builtin_provider_definitions();
    auto second = ai::builtin_provider_definitions();

    REQUIRE(first.size() == second.size());
    REQUIRE(first.size() == 6);

    auto first_codex_it =
            std::find_if(first.begin(), first.end(), [](const auto& d) { return d.id == "openai-codex"; });
    auto first_kimi_it = std::find_if(first.begin(), first.end(), [](const auto& d) { return d.id == "kimi-coding"; });
    REQUIRE(first_codex_it != first.end());
    REQUIRE(first_kimi_it != first.end());

    first_codex_it->name = "mutated";
    first_codex_it->models.front().name = "mutated";
    first_codex_it->auth.oauth->name = "mutated";
    first_kimi_it->models.front().name = "mutated";
    first_kimi_it->auth.api_key->name = "mutated";

    auto second_codex_it =
            std::find_if(second.begin(), second.end(), [](const auto& d) { return d.id == "openai-codex"; });
    auto second_kimi_it =
            std::find_if(second.begin(), second.end(), [](const auto& d) { return d.id == "kimi-coding"; });
    REQUIRE(second_codex_it != second.end());
    REQUIRE(second_kimi_it != second.end());

    CHECK(second_codex_it->name == "OpenAI Codex");
    CHECK(second_codex_it->models.front().name == "GPT-5.3 Codex Spark");
    CHECK(second_codex_it->auth.oauth->name == "OpenAI (ChatGPT Plus/Pro)");
    CHECK(second_kimi_it->models.front().name == "Kimi K3");
    CHECK(second_kimi_it->auth.api_key->name == "Kimi API key");
}

namespace {

/// The T0 provenance record owns the exhaustive model IDs. This smaller
/// catalog seam check keeps the shipped provider counts and API families
/// visible while the independent exhaustive parity test remains in T7.
struct PinnedProvider {
    std::string_view id;
    std::size_t model_count;
    std::set<std::string_view> apis;
};

} // namespace

TEST_CASE("the generated catalog pins provider counts API families and limits", "[ai][providers][issue760][spec]") {
    const std::vector<PinnedProvider> pinned{
            {"deepseek", 2, {"openai-completions"}},
            {"kimi-coding", 4, {"openai-completions"}},
            {"openai", 39, {"openai-responses"}},
            {"openai-codex", 6, {"openai-codex-responses"}},
            {"openrouter", 378, {"anthropic-messages", "openai-completions"}},
            {"opencode-go", 30, {"anthropic-messages", "openai-completions", "openai-responses"}},
    };

    const auto definitions = ai::builtin_provider_definitions();
    REQUIRE(definitions.size() == pinned.size());

    for (const auto& want : pinned) {
        const auto it = std::find_if(definitions.begin(), definitions.end(), [&](const auto& definition) {
            return definition.id == want.id;
        });
        REQUIRE(it != definitions.end());

        CHECK(it->models.size() == want.model_count);
        std::set<std::string_view> apis;
        for (const auto& model : it->models) {
            apis.insert(model.api);
            CHECK_FALSE(model.base_url.empty());
            CHECK_FALSE(model.input.empty());
            CHECK(model.context_window > 0);
            CHECK(model.max_tokens > 0);
        }
        CHECK(apis == want.apis);
    }

    const auto kimi_it =
            std::find_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.id == "kimi-coding"; });
    REQUIRE(kimi_it != definitions.end());
    const auto kimi_model = std::find_if(kimi_it->models.begin(), kimi_it->models.end(), [](const auto& model) {
        return model.id == "kimi-for-coding";
    });
    REQUIRE(kimi_model != kimi_it->models.end());
    CHECK(kimi_model->base_url == "https://api.kimi.com/coding/v1");
    CHECK(kimi_model->context_window == 1048576);
    CHECK(kimi_model->max_tokens == 32768);
    CHECK(kimi_model->input == std::vector<ai::ModelInput>{ai::ModelInput::Text, ai::ModelInput::Image});
    CHECK_FALSE(kimi_model->headers.has_value());
    REQUIRE(kimi_model->thinking_level_map);
    CHECK(kimi_model->thinking_level_map->at(ai::ModelThinkingLevel::Low) == "low");
    CHECK(kimi_model->thinking_level_map->at(ai::ModelThinkingLevel::High) == "high");
    CHECK(kimi_model->thinking_level_map->at(ai::ModelThinkingLevel::Max) == "max");
    CHECK(kimi_model->thinking_level_map->at(ai::ModelThinkingLevel::Medium) == std::nullopt);

    const auto deepseek_it =
            std::find_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.id == "deepseek"; });
    REQUIRE(deepseek_it != definitions.end());
    const auto deepseek_model = std::find_if(
            deepseek_it->models.begin(),
            deepseek_it->models.end(),
            [](const ai::Model& model) { return model.id == "deepseek-flash"; });
    REQUIRE(deepseek_model != deepseek_it->models.end());
    REQUIRE(deepseek_model->compat);
    const auto* deepseek_compat =
            std::get_if<ai::OpenAICompletionsCompat>(&*deepseek_model->compat);
    REQUIRE(deepseek_compat != nullptr);
    CHECK(deepseek_compat->supports_strict_mode == true);

    const auto codex_it =
            std::find_if(definitions.begin(), definitions.end(), [](const auto& d) { return d.id == "openai-codex"; });
    REQUIRE(codex_it != definitions.end());
    const auto codex_model = std::find_if(
            codex_it->models.begin(), codex_it->models.end(), [](const auto& model) { return model.id == "gpt-5.5"; });
    REQUIRE(codex_model != codex_it->models.end());
    REQUIRE(codex_model->cost.tiers);
    CHECK(codex_model->cost.tiers->front().input_tokens_above == 272000);
}

TEST_CASE("the generated catalog preserves published token rates and zero-cost sentinels",
        "[ai][providers][issue760][spec]") {
    for (const auto& definition : ai::builtin_provider_definitions()) {
        for (const auto& model : definition.models) {
            const bool has_rates = model.cost.input > 0.0 || model.cost.output > 0.0 || model.cost.cache_read > 0.0 ||
                                   model.cost.cache_write > 0.0;
            const bool is_unpriced_kimi = model.id == "k3-256k";
            const bool is_provider_routed_openrouter =
                    model.id == "openrouter/auto" || model.id == "openrouter/auto-beta";
            const bool is_zero_cost_openrouter =
                    definition.id == "openrouter" &&
                    (model.id == "auto" || std::string_view{model.id}.ends_with(":free") ||
                            model.id == "openrouter/free" || model.id == "openrouter/fusion");
            CHECK((has_rates || is_unpriced_kimi || is_provider_routed_openrouter || is_zero_cost_openrouter));
            if (is_provider_routed_openrouter) {
                CHECK(model.cost.input == -1000000.0);
                CHECK(model.cost.output == -1000000.0);
            }
            if (is_zero_cost_openrouter) {
                CHECK(model.cost.input == 0.0);
                CHECK(model.cost.output == 0.0);
                CHECK(model.cost.cache_read == 0.0);
                CHECK(model.cost.cache_write == 0.0);
            }
        }
    }
}
