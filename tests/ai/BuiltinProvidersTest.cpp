#include <cch/ai/Models.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <concepts>
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
    REQUIRE(codex.models.size() == 7);
    CHECK(codex.models.front().id == "gpt-5.3-codex-spark");
    CHECK(codex.models.back().id == "gpt-5.6-terra");
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

/// One built-in provider's pinned wire surface (issue #754): the base URL, the
/// API, and the model order the bundled catalog must produce. A typo in the
/// embedded document changes one of these and fails here.
struct PinnedProvider {
    std::string_view id;
    std::string_view base_url;
    std::string_view api;
    std::vector<std::string_view> model_ids;
};

} // namespace

TEST_CASE("the bundled catalog pins each provider's wire surface and limits", "[ai][providers][issue754][spec]") {
    const std::vector<PinnedProvider> pinned{
            {"deepseek",
                    "https://api.deepseek.com/responses",
                    "openai-responses",
                    {"deepseek-chat", "deepseek-reasoner"}},
            {"openrouter",
                    "https://openrouter.ai/api/v1/responses",
                    "openai-responses",
                    {"anthropic/claude-3.7-sonnet", "deepseek/deepseek-r1", "openai/gpt-4o"}},
            {"opencode-go",
                    "https://opencode.ai/zen/go/v1/responses",
                    "openai-responses",
                    {"gpt-5.6-luna", "grok-4.5"}},
            {"openai",
                    "https://api.openai.com/v1/responses",
                    "openai-responses",
                    {"gpt-4o", "gpt-4o-mini", "o1", "o3-mini"}},
            {"openai-codex",
                    "https://chatgpt.com/backend-api",
                    "openai-codex-responses",
                    {"gpt-5.3-codex-spark",
                            "gpt-5.4",
                            "gpt-5.4-mini",
                            "gpt-5.5",
                            "gpt-5.6-luna",
                            "gpt-5.6-sol",
                            "gpt-5.6-terra"}},
            {"kimi-coding",
                    "https://api.kimi.com/coding",
                    "anthropic-messages",
                    {"k3", "k3-256k", "kimi-for-coding", "kimi-for-coding-highspeed"}},
    };

    const auto definitions = ai::builtin_provider_definitions();
    REQUIRE(definitions.size() == pinned.size());

    for (const auto& want : pinned) {
        const auto it = std::find_if(definitions.begin(), definitions.end(), [&](const auto& definition) {
            return definition.id == want.id;
        });
        REQUIRE(it != definitions.end());

        std::vector<std::string_view> model_ids;
        model_ids.reserve(it->models.size());
        for (const auto& model : it->models) {
            model_ids.push_back(model.id);
            CHECK(model.base_url == want.base_url);
            CHECK(model.api == want.api);
            // The catalog states the limits explicitly; the parser's fallbacks
            // are a safety net, never the intended value.
            CHECK(model.context_window > 0);
            CHECK(model.max_tokens > 0);
        }
        CHECK(model_ids == want.model_ids);
    }
}

TEST_CASE("every bundled model except k3-256k carries token rates", "[ai][providers][issue754][spec]") {
    // k3-256k ships without published rates, so the catalog leaves its cost
    // table as zeros. Every other model must carry at least one non-zero rate:
    // a silently zeroed table would otherwise look like a free model.
    for (const auto& definition : ai::builtin_provider_definitions()) {
        for (const auto& model : definition.models) {
            const bool has_rates = model.cost.input > 0.0 || model.cost.output > 0.0 || model.cost.cache_read > 0.0 ||
                                   model.cost.cache_write > 0.0;
            CHECK(has_rates == (model.id != "k3-256k"));
        }
    }
}
