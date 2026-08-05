#include <cch/ai/Model.hpp>
#include <cch/ai/Provider.hpp>
#include "ai/glaze/ModelJson.hpp"
#include "coding_agent/ModelConfig.hpp"
#include "coding_agent/ProviderComposer.hpp"
#include "support/JsonCompare.hpp"
#include "support/PiFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ProviderComposer: built-in/config composition (pi provider-composer subset)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("builtin_providers ships the Codex 7 and Kimi 4 catalogs", "[coding_agent][provider-composer][issue345]") {
    const auto builtins = coding_agent::builtin_providers(composer_options());
    REQUIRE(builtins.count("openai-codex") == 1);
    REQUIRE(builtins.count("kimi-coding") == 1);

    const auto codex = builtins.at("openai-codex");
    CHECK(codex->name() == "OpenAI Codex");
    CHECK(codex->auth().oauth.has_value());
    CHECK_FALSE(codex->auth().api_key.has_value());
    const auto codex_models = codex->models();
    REQUIRE(codex_models.size() == 7);
    CHECK(codex_models.front().id == "gpt-5.3-codex-spark");
    CHECK(codex_models.back().id == "gpt-5.6-terra");
    const auto gpt55 = std::find_if(codex_models.begin(), codex_models.end(),
        [](const ai::Model& m) { return m.id == "gpt-5.5"; });
    REQUIRE(gpt55 != codex_models.end());
    CHECK(gpt55->api == "openai-codex-responses");
    CHECK(gpt55->base_url == "https://chatgpt.com/backend-api");

    const auto kimi = builtins.at("kimi-coding");
    CHECK(kimi->name() == "Kimi For Coding");
    CHECK(kimi->auth().api_key.has_value());
    CHECK(kimi->auth().oauth.has_value());
    const auto kimi_models = kimi->models();
    REQUIRE(kimi_models.size() == 4);
    const auto kimi_coding = std::find_if(kimi_models.begin(), kimi_models.end(),
        [](const ai::Model& m) { return m.id == "kimi-for-coding"; });
    REQUIRE(kimi_coding != kimi_models.end());
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
        const auto* by_api = shard->get_if<util::JsonValue::object_t>();
        REQUIRE(by_api);
        const auto found = by_api->find(std::string{api});
        REQUIRE(found != by_api->end());
        const auto* golden = found->second.get_if<util::JsonValue::object_t>();
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
            const auto actual = util::read_json<util::JsonValue>(*serialized);
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

    const auto builtins = coding_agent::builtin_providers(composer_options());
    const auto codex = builtins.at("openai-codex");
    check_shard(codex->models(), "models/openai-codex-shard.json", "openai-codex-responses");
    const auto kimi = builtins.at("kimi-coding");
    check_shard(kimi->models(), "models/kimi-coding-shard.json", "anthropic-messages");
}

TEST_CASE("built-in without models.json config is used untouched", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({"providers": {}})");
    const auto builtins = coding_agent::builtin_providers(composer_options());
    std::optional<std::string> error;
    auto composed = coding_agent::compose_provider(
        "openai-codex", builtins.at("openai-codex"), config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(composed != nullptr);
    CHECK(composed.get() == builtins.at("openai-codex").get());
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
    const auto builtins = coding_agent::builtin_providers(composer_options());
    std::optional<std::string> error;
    auto composed = coding_agent::compose_provider(
        "openai-codex", builtins.at("openai-codex"), config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(composed != nullptr);

    const auto models = composed->models();
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
    CHECK(composed->auth().oauth.has_value());
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
    const auto builtins = coding_agent::builtin_providers(composer_options());
    std::optional<std::string> error;
    auto composed = coding_agent::compose_provider(
        "kimi-coding", builtins.at("kimi-coding"), config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(composed != nullptr);
    const auto models = composed->models();
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
    auto composed = coding_agent::compose_provider(
        "deepseek", nullptr, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(composed != nullptr);
    CHECK(composed->name() == "DeepSeek");
    CHECK(composed->auth().api_key.has_value());
    const auto models = composed->models();
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
    auto composed = coding_agent::compose_provider(
        "deepseek", nullptr, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(composed != nullptr);
    REQUIRE(composed->auth().api_key.has_value());
}

TEST_CASE("composition failure falls back to the built-in and records the error", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    // kimi-coding overlay with no useful content: "must specify baseUrl/..."
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "kimi-coding": {"name": "Broken Kimi"}
      }
    })");
    const auto builtins = coding_agent::builtin_providers(composer_options());
    std::optional<std::string> error;
    auto composed = coding_agent::compose_provider(
        "kimi-coding", builtins.at("kimi-coding"), config, composer_options(), error);
    REQUIRE(error.has_value());
    CHECK(error->find("must specify") != std::string::npos);
    // Built-in fallback is returned.
    REQUIRE(composed != nullptr);
    CHECK(composed.get() == builtins.at("kimi-coding").get());
}

TEST_CASE("absent provider with no config returns null", "[coding_agent][provider-composer][issue345]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({"providers": {}})");
    std::optional<std::string> error;
    auto composed = coding_agent::compose_provider(
        "deepseek", nullptr, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    CHECK(composed == nullptr);
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
    auto composed = coding_agent::compose_provider(
        "deepseek", nullptr, config, composer_options(), error);
    REQUIRE(error.has_value());
    CHECK(error->find("no \"api\" specified") != std::string::npos);
    CHECK(composed == nullptr);
}

TEST_CASE("default-model table maps the supported provider subset", "[coding_agent][provider-composer][issue345]") {
    CHECK(coding_agent::default_model_for_provider("openai-codex") == std::optional<std::string>{"gpt-5.5"});
    CHECK(coding_agent::default_model_for_provider("kimi-coding") == std::optional<std::string>{"kimi-for-coding"});
    // Config-only deepseek has no default model (the "no default model" branch).
    CHECK_FALSE(coding_agent::default_model_for_provider("deepseek").has_value());
    CHECK_FALSE(coding_agent::default_model_for_provider("unknown-provider").has_value());
}
