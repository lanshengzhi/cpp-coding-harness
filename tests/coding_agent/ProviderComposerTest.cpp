#include <cch/ai/Models.hpp>
#include "coding_agent/ModelConfig.hpp"
#include "coding_agent/ProviderComposer.hpp"
#include "support/ModelFixture.hpp"
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

/// Compose fixture `models.json` model entries through the production config
/// path: a config-only provider carrying the entries is loaded by `ModelConfig`
/// and turned into `ai::Model` values by `compose_provider`.
[[nodiscard]] std::vector<ai::Model> compose_fixture_models(
        const tests::TempWorkspace& workspace, std::string_view provider_id, const std::vector<std::string>& entries) {
    std::string models;
    for (const auto& entry : entries) {
        if (!models.empty()) {
            models += ',';
        }
        models += entry;
    }
    const auto config = load_models_json(workspace,
            R"({"providers":{")" + std::string{provider_id} + R"(":{"apiKey":"dummy-fixture-key","models":[)" + models +
                    "]}}}");
    REQUIRE_FALSE(config.error().has_value());
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider(provider_id, std::nullopt, config, composer_options(), error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    return change.definition->models;
}

[[nodiscard]] std::optional<std::string> string_field_mismatch(
        std::string_view field, const std::string& expected, const std::string& actual) {
    if (expected == actual) {
        return std::nullopt;
    }
    return std::string{field} + ": expected \"" + expected + "\", composed \"" + actual + "\"";
}

/// First field difference between a built-in catalog `ai::Model` and the
/// `ai::Model` composed from the same frozen shard entry, or nullopt when they
/// match. `compat` is not compared: the C++ `models.json` schema has no compat
/// surface (`ModelConfig.hpp`), so a shard entry's compat member never reaches
/// the config path; `AnthropicMessagesAdapterTest` pins the Kimi catalog compat
/// values instead.
[[nodiscard]] std::optional<std::string> model_field_mismatch(const ai::Model& expected, const ai::Model& actual) {
    if (auto mismatch = string_field_mismatch("id", expected.id, actual.id)) {
        return mismatch;
    }
    if (auto mismatch = string_field_mismatch("name", expected.name, actual.name)) {
        return mismatch;
    }
    if (auto mismatch = string_field_mismatch("api", expected.api, actual.api)) {
        return mismatch;
    }
    if (auto mismatch = string_field_mismatch("provider", expected.provider, actual.provider)) {
        return mismatch;
    }
    if (auto mismatch = string_field_mismatch("baseUrl", expected.base_url, actual.base_url)) {
        return mismatch;
    }
    if (expected.reasoning != actual.reasoning) {
        return "reasoning differs";
    }
    if (expected.thinking_level_map != actual.thinking_level_map) {
        return "thinkingLevelMap differs";
    }
    if (expected.input != actual.input) {
        return "input differs";
    }
    if (expected.cost.input != actual.cost.input || expected.cost.output != actual.cost.output ||
            expected.cost.cache_read != actual.cost.cache_read ||
            expected.cost.cache_write != actual.cost.cache_write) {
        return "cost rates differ";
    }
    if (expected.cost.tiers.has_value() != actual.cost.tiers.has_value()) {
        return "cost tiers presence differs";
    }
    if (expected.cost.tiers.has_value()) {
        if (expected.cost.tiers->size() != actual.cost.tiers->size()) {
            return "cost tier count differs";
        }
        for (std::size_t index = 0; index < expected.cost.tiers->size(); ++index) {
            const auto& expected_tier = (*expected.cost.tiers)[index];
            const auto& actual_tier = (*actual.cost.tiers)[index];
            if (expected_tier.input != actual_tier.input || expected_tier.output != actual_tier.output ||
                    expected_tier.cache_read != actual_tier.cache_read ||
                    expected_tier.cache_write != actual_tier.cache_write ||
                    expected_tier.input_tokens_above != actual_tier.input_tokens_above) {
                return "cost tier " + std::to_string(index) + " differs";
            }
        }
    }
    if (expected.context_window != actual.context_window) {
        return "contextWindow differs";
    }
    if (expected.max_tokens != actual.max_tokens) {
        return "maxTokens differs";
    }
    if (expected.headers != actual.headers) {
        return "headers differs";
    }
    return std::nullopt;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ProviderComposer: built-in/config composition (pi provider-composer subset)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("builtin definitions carry the Codex 7 and Kimi 4 catalogs",
        "[coding_agent][provider-composer][issue546][spec]") {
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

TEST_CASE("builtin catalogs match the frozen baseline shard values",
        "[coding_agent][provider-composer][issue370][spec]") {
    // The committed shard goldens (fixtures/pi-ai/models/*-shard.json) are
    // verbatim copies of the frozen-baseline pi shards (byte-hashes pinned in
    // the pi-ai fixture README). Every built-in catalog model must equal the
    // `ai::Model` the production models.json path composes from its baseline
    // shard entry. The shard compat members are excluded (see
    // `model_field_mismatch`); the deferred Codex catalog compat flags
    // (supportsOpenAIGrammarTools / supportsToolSearch) remain absent from the
    // C++ surface by design.
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

        REQUIRE_FALSE(models.empty());
        REQUIRE(models.size() == golden->size());
        std::vector<std::string> entries;
        entries.reserve(golden->size());
        for (const auto& golden_entry : *golden) {
            const auto entry_json = support::write_json(golden_entry.second);
            REQUIRE(entry_json);
            entries.push_back(*entry_json);
        }
        tests::TempWorkspace workspace;
        const auto composed = compose_fixture_models(workspace, models.front().provider, entries);
        REQUIRE(composed.size() == models.size());

        for (const auto& model : models) {
            const auto match = std::find_if(composed.begin(), composed.end(), [&model](const ai::Model& value) {
                return value.id == model.id;
            });
            REQUIRE(match != composed.end());
            if (auto mismatch = model_field_mismatch(model, *match); mismatch) {
                // The vendored fallback test header has no INFO macro; print
                // the diff to stderr so it appears in the failure output.
                std::cerr << "CATALOG SHARD MISMATCH (" << shard_fixture << "): " << model.id << "\n"
                          << *mismatch << "\n";
                CHECK(false);
            }
        }
    };

    const auto builtins = ai::builtin_provider_definitions();
    REQUIRE(builtins.size() == 2);
    check_shard(builtins[0].models, "models/openai-codex-shard.json", "openai-codex-responses");
    check_shard(builtins[1].models, "models/kimi-coding-shard.json", "anthropic-messages");
}

TEST_CASE("the frozen complete Model fixture composes to the expected ai::Model",
        "[coding_agent][provider-composer][issue336][compat-pi]") {
    // complete-anthropic-model.json exercises every supported Model field. The
    // vendored entry is read through the production config path, so the golden
    // pins what `models.json` composition must produce rather than a test-only
    // write surface. The entry's `provider` member has no config meaning (the
    // provider key supplies it) and its `compat` member has no C++ models.json
    // surface, so the expected value carries no compat.
    const auto fixture = tests::read_pi_fixture_text("models/complete-anthropic-model.json");
    REQUIRE(fixture);
    tests::TempWorkspace workspace;
    const auto composed = compose_fixture_models(workspace, "kimi-coding", {*fixture});
    REQUIRE(composed.size() == 1);

    auto expected = tests::make_model("kimi-for-coding", "kimi-coding", "anthropic-messages");
    expected.name = "Kimi for Coding";
    expected.base_url = "https://api.kimi.com/coding";
    expected.reasoning = true;
    expected.thinking_level_map = ai::ThinkingLevelMap{
            {ai::ModelThinkingLevel::Minimal, std::string{"low"}},
            {ai::ModelThinkingLevel::Low, std::nullopt},
            {ai::ModelThinkingLevel::High, std::string{"high"}},
    };
    expected.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    expected.cost = ai::ModelCost{
            .input = 1.0,
            .output = 4.0,
            .cache_read = 0.1,
            .cache_write = 1.25,
            .tiers = std::vector<ai::ModelCostTier>{ai::ModelCostTier{
                    .input = 2.0,
                    .output = 8.0,
                    .cache_read = 0.2,
                    .cache_write = 2.5,
                    .input_tokens_above = 200000,
            }},
    };
    expected.context_window = 262144;
    expected.max_tokens = 32768;
    expected.headers = ai::ModelHeaders{{"X-Static", "catalog"}};

    if (auto mismatch = model_field_mismatch(expected, composed.front()); mismatch) {
        std::cerr << "COMPLETE MODEL FIXTURE MISMATCH\n" << *mismatch << "\n";
        CHECK(false);
    }
}

TEST_CASE("built-in without models.json config is submitted unchanged",
        "[coding_agent][provider-composer][issue546][spec]") {
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

TEST_CASE("models.json overlay overrides the built-in baseUrl and upserts a custom model",
        "[coding_agent][provider-composer][issue345][spec]") {
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

TEST_CASE("model overrides apply last over the composed model", "[coding_agent][provider-composer][issue345][spec]") {
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

TEST_CASE("config-only provider composes from models.json plus the openai-responses adapter",
        "[coding_agent][provider-composer][issue345][spec]") {
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

TEST_CASE("config-only provider without apiKey still composes but resolves no auth",
        "[coding_agent][provider-composer][issue345][spec]") {
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

TEST_CASE("composition failure falls back to the built-in and records the error",
        "[coding_agent][provider-composer][issue345][spec]") {
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

TEST_CASE("absent provider with no config returns null", "[coding_agent][provider-composer][issue345][spec]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({"providers": {}})");
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("deepseek", std::nullopt, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    CHECK(change.provider_id == "deepseek");
    CHECK_FALSE(change.definition.has_value());
}

TEST_CASE("custom model requires an api and baseUrl", "[coding_agent][provider-composer][issue345][spec]") {
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

TEST_CASE(
        "default-model table maps the supported provider subset", "[coding_agent][provider-composer][issue345][spec]") {
    CHECK(coding_agent::default_model_for_provider("openai-codex") == std::optional<std::string>{"gpt-5.5"});
    CHECK(coding_agent::default_model_for_provider("kimi-coding") == std::optional<std::string>{"kimi-for-coding"});
    // Config-only deepseek has no default model (the "no default model" branch).
    CHECK_FALSE(coding_agent::default_model_for_provider("deepseek").has_value());
    CHECK_FALSE(coding_agent::default_model_for_provider("unknown-provider").has_value());
}
