#include <cch/ai/Models.hpp>
#include "ai/glaze/ModelJson.hpp"
#include "coding_agent/ModelConfig.hpp"
#include "coding_agent/ProviderComposer.hpp"
#include "support/JsonCompare.hpp"
#include "support/PiFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::ModelConfig load_models_json(
    const tests::TempWorkspace& workspace,
    std::string content) {
    const auto path = workspace.path() / "models.json";
    std::ofstream output(path);
    output << content;
    output.close();
    return coding_agent::ModelConfig::load(path);
}

[[nodiscard]] coding_agent::ProviderComposerOptions composer_options() {
    return coding_agent::ProviderComposerOptions{};
}

[[nodiscard]] std::optional<ai::ProviderDefinition> builtin_definition(std::string_view provider_id) {
    for (auto&& definition : ai::builtin_provider_definitions()) {
        if (definition.id == provider_id) {
            return std::move(definition);
        }
    }
    return std::nullopt;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ProviderComposer: built-in/config composition (pi provider-composer subset)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("builtin definitions carry the Codex 7 and Kimi 4 catalogs", "[coding_agent][provider-composer][issue546]") {
    const auto builtins = ai::builtin_provider_definitions();
    REQUIRE(builtins.size() == 2);

    const auto& codex = builtins[0];
    CHECK(codex.id == "openai-codex");
    CHECK(codex.name == "OpenAI Codex");
    CHECK(codex.auth.oauth.has_value());
    CHECK_FALSE(codex.auth.api_key.has_value());
    REQUIRE(codex.models.size() == 7);
    CHECK(codex.models.front().id == "gpt-5.3-codex-spark");
    CHECK(codex.models.back().id == "gpt-5.6-terra");
    const auto gpt55 = std::find_if(
            codex.models.begin(), codex.models.end(), [](const ai::Model& m) { return m.id == "gpt-5.5"; });
    REQUIRE(gpt55 != codex.models.end());
    CHECK(gpt55->api == "openai-codex-responses");
    CHECK(gpt55->base_url == "https://chatgpt.com/backend-api");

    const auto& kimi = builtins[1];
    CHECK(kimi.id == "kimi-coding");
    CHECK(kimi.name == "Kimi For Coding");
    CHECK(kimi.auth.api_key.has_value());
    CHECK(kimi.auth.oauth.has_value());
    REQUIRE(kimi.models.size() == 4);
    const auto kimi_coding = std::find_if(
            kimi.models.begin(), kimi.models.end(), [](const ai::Model& m) { return m.id == "kimi-for-coding"; });
    REQUIRE(kimi_coding != kimi.models.end());
    CHECK(kimi_coding->api == "anthropic-messages");
    CHECK(kimi_coding->compat.has_value());
    CHECK(kimi_coding->compat->allow_empty_signature == true);
}

TEST_CASE(
    "builtin catalogs match the frozen baseline shard values",
    "[coding_agent][provider-composer][issue370]") {
    // The committed shard goldens (fixtures/pi-ai/models/*-shard.json) are
    // verbatim copies of the frozen-baseline pi shards (byte-hashes pinned in
    // the pi-ai fixture README). Every built-in catalog model must serialize
    // to exactly its baseline shard entry, except the deferred Codex catalog
    // compat flags (supportsOpenAIGrammarTools / supportsToolSearch), which
    // are absent from the C++ surface and are stripped from the golden entry.
    const auto check_shard = [](const std::vector<ai::Model>& models,
                                std::string_view shard_fixture,
                                std::string_view api) {
        const auto shard = tests::read_pi_fixture(shard_fixture);
        REQUIRE(shard);
        const auto* by_api = shard->get_if<support::JsonValue::object_t>();
        REQUIRE(by_api);
        const auto found = by_api->find(std::string{api});
        REQUIRE(found != by_api->end());
        const auto* golden = found->second.get_if<support::JsonValue::object_t>();
        REQUIRE(golden);

        REQUIRE(models.size() == golden->size());
        for (const auto& model : models) {
            const auto golden_entry = golden->find(model.id);
            REQUIRE(golden_entry != golden->end());
            auto expected = golden_entry->second;
            if (!model.compat) {
                expected.get_object().erase("compat");
            }
            auto serialized = ai::glaze::write_model_json(model);
            REQUIRE(serialized);
            const auto actual = support::read_json(*serialized);
            REQUIRE(actual);
            if (auto mismatch = tests::json_mismatch(expected, *actual); mismatch) {
                // The vendored fallback test header has no INFO macro; print
                // the diff to stderr so it appears in the failure output.
                std::cerr << "CATALOG SHARD MISMATCH (" << shard_fixture << ")"
                          << ":\n" << *mismatch << "\n";
                CHECK(false);
            }
        }
    };

    const auto builtins = ai::builtin_provider_definitions();
    REQUIRE(builtins.size() == 2);
    check_shard(builtins[0].models, "models/openai-codex-shard.json", "openai-codex-responses");
    check_shard(builtins[1].models, "models/kimi-coding-shard.json", "anthropic-messages");
}

TEST_CASE("built-in without models.json config is submitted unchanged", "[coding_agent][provider-composer][issue546]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({"providers": {}})");
    auto base = builtin_definition("openai-codex");
    REQUIRE(base.has_value());
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("openai-codex", std::move(base), config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    CHECK(change.provider_id == "openai-codex");
    REQUIRE(change.definition.has_value());
    CHECK(change.definition->id == "openai-codex");
    CHECK(change.definition->name == "OpenAI Codex");
    CHECK(change.definition->models.size() == 7);
    CHECK(change.definition->auth.oauth.has_value());
}

TEST_CASE("models.json overlay overrides the built-in baseUrl and upserts a custom model", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "openai-codex": {
          "baseUrl": "https://codex.example/v1",
          "models": [{
            "id": "gpt-5.5",
            "name": "GPT-5.5 Override",
            "maxTokens": 65536
          }]
        }
      }
    })");
    auto base = builtin_definition("openai-codex");
    REQUIRE(base.has_value());
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("openai-codex", std::move(base), config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());

    const auto& models = change.definition->models;
    // Same-id custom-model upsert replaces gpt-5.5.
    const auto gpt55 = std::find_if(models.begin(), models.end(),
        [](const ai::Model& m) { return m.id == "gpt-5.5"; });
    REQUIRE(gpt55 != models.end());
    CHECK(gpt55->name == "GPT-5.5 Override");
    CHECK(gpt55->max_tokens == 65536);
    CHECK(gpt55->base_url == "https://codex.example/v1");
    // The overlay's baseUrl propagates to every built-in model.
    const auto gpt54 = std::find_if(models.begin(), models.end(),
        [](const ai::Model& m) { return m.id == "gpt-5.4"; });
    REQUIRE(gpt54 != models.end());
    CHECK(gpt54->base_url == "https://codex.example/v1");
    // The built-in OAuth auth is preserved.
    CHECK(change.definition->auth.oauth.has_value());
}

TEST_CASE("model overrides apply last over the composed model", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "kimi-coding": {
          "modelOverrides": {
            "kimi-for-coding": {"name": "Kimi Override", "reasoning": false}
          }
        }
      }
    })");
    auto base = builtin_definition("kimi-coding");
    REQUIRE(base.has_value());
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("kimi-coding", std::move(base), config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    const auto& models = change.definition->models;
    const auto target = std::find_if(models.begin(), models.end(),
        [](const ai::Model& m) { return m.id == "kimi-for-coding"; });
    REQUIRE(target != models.end());
    CHECK(target->name == "Kimi Override");
    CHECK(target->reasoning == false);
}

TEST_CASE("config-only provider composes from models.json plus the openai-responses adapter", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "name": "DeepSeek",
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "dummy-deepseek-key",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("deepseek", std::nullopt, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    CHECK(change.definition->name == "DeepSeek");
    CHECK(change.definition->auth.api_key.has_value());
    const auto& models = change.definition->models;
    REQUIRE(models.size() == 1);
    CHECK(models.front().id == "deepseek-v4-flash");
    CHECK(models.front().api == "openai-responses");
    CHECK(models.front().provider == "deepseek");
    CHECK(models.front().base_url == "https://api.deepseek.example/v1");
    CHECK(models.front().input == std::vector<ai::ModelInput>{ai::ModelInput::Text});
}

TEST_CASE("config-only provider without apiKey still composes but resolves no auth", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("deepseek", std::nullopt, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    REQUIRE(change.definition->auth.api_key.has_value());
}

TEST_CASE("composition failure falls back to the built-in and records the error", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    // kimi-coding overlay with no useful content: "must specify baseUrl/..."
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "kimi-coding": {"name": "Broken Kimi"}
      }
    })");
    auto base = builtin_definition("kimi-coding");
    REQUIRE(base.has_value());
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("kimi-coding", std::move(base), config, composer_options(), error);
    REQUIRE(error.has_value());
    CHECK(error->find("must specify") != std::string::npos);
    // The complete built-in definition is returned as the fallback.
    REQUIRE(change.definition.has_value());
    CHECK(change.definition->id == "kimi-coding");
    CHECK(change.definition->name == "Kimi For Coding");
    CHECK(change.definition->models.size() == 4);
    CHECK(change.definition->auth.oauth.has_value());
    CHECK(change.definition->auth.api_key.has_value());
}

TEST_CASE("absent provider with no config returns null", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({"providers": {}})");
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("deepseek", std::nullopt, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    CHECK(change.provider_id == "deepseek");
    CHECK_FALSE(change.definition.has_value());
}

TEST_CASE("custom model requires an api and baseUrl", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    // Model with neither api nor baseUrl.
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "models": [{"id": "deepseek-v4-flash"}]
        }
      }
    })");
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("deepseek", std::nullopt, config, composer_options(), error);
    REQUIRE(error.has_value());
    CHECK(error->find("no \"api\" specified") != std::string::npos);
    CHECK_FALSE(change.definition.has_value());
}

TEST_CASE("default-model table maps the supported provider subset", "[coding_agent][provider-composer][issue345]") {
    CHECK(coding_agent::default_model_for_provider("openai-codex") == std::optional<std::string>{"gpt-5.5"});
    CHECK(coding_agent::default_model_for_provider("kimi-coding") == std::optional<std::string>{"kimi-for-coding"});
    // Config-only deepseek has no default model (the "no default model" branch).
    CHECK_FALSE(coding_agent::default_model_for_provider("deepseek").has_value());
    CHECK_FALSE(coding_agent::default_model_for_provider("unknown-provider").has_value());
}
