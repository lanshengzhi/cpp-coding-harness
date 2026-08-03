#include <cch/ai/Model.hpp>
#include "coding_agent/ModelConfig.hpp"
#include "support/TempWorkspace.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <filesystem>
#include <optional>
#include <string>

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ModelConfig: models.json parsing and validation (pi ModelConfig subset)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ModelConfig missing file resolves to empty user config without diagnostics", "[coding_agent][model-config][issue345]") {
    tests::TempWorkspace workspace;
    auto config = coding_agent::ModelConfig::load(workspace.path() / "missing.json");
    CHECK(config.empty());
    CHECK_FALSE(config.error().has_value());
    CHECK(config.provider_ids().empty());
}

TEST_CASE("ModelConfig invalid JSON records a parse diagnostic with an empty config", "[coding_agent][model-config][issue345]") {
    tests::TempWorkspace workspace;
    auto config = load_models_json(workspace, "{not valid json");
    CHECK(config.empty());
    REQUIRE(config.error().has_value());
    CHECK(config.error()->find("Failed to parse models.json") != std::string::npos);
}

TEST_CASE("ModelConfig schema violation records an invalid-schema diagnostic", "[coding_agent][model-config][issue345]") {
    tests::TempWorkspace workspace;
    // Model definition without the required "id".
    auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "models": [{"api": "openai-responses"}]
        }
      }
    })");
    CHECK(config.empty());
    REQUIRE(config.error().has_value());
    CHECK(config.error()->find("Invalid models.json schema") != std::string::npos);
    CHECK(config.error()->find("models[0].id") != std::string::npos);
}

TEST_CASE("ModelConfig parses a config-only provider with a custom model", "[coding_agent][model-config][issue345]") {
    tests::TempWorkspace workspace;
    auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "name": "DeepSeek",
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "dummy-deepseek-key",
          "headers": {"X-Test": "value"},
          "models": [{
            "id": "deepseek-v4-flash",
            "name": "DeepSeek V4 Flash",
            "reasoning": true,
            "input": ["text"],
            "contextWindow": 131072,
            "maxTokens": 32768,
            "cost": {"input": 2.0, "output": 4.0, "cacheRead": 1.0, "cacheWrite": 3.0}
          }],
          "modelOverrides": {"deepseek-v4-flash": {"maxTokens": 65536}}
        }
      }
    })");
    CHECK_FALSE(config.empty());
    CHECK_FALSE(config.error().has_value());

    const auto ids = config.provider_ids();
    REQUIRE(ids.size() == 1);
    CHECK(ids.front() == "deepseek");

    const auto provider = config.provider("deepseek");
    REQUIRE(provider.has_value());
    CHECK(*provider->name == "DeepSeek");
    CHECK(*provider->base_url == "https://api.deepseek.example/v1");
    CHECK(*provider->api == "openai-responses");
    CHECK(*provider->api_key == "dummy-deepseek-key");
    REQUIRE(provider->headers.has_value());
    CHECK(provider->headers->at("X-Test") == "value");

    REQUIRE(provider->models.has_value());
    REQUIRE(provider->models->size() == 1);
    const auto& model = provider->models->front();
    CHECK(model.id == "deepseek-v4-flash");
    CHECK(*model.name == "DeepSeek V4 Flash");
    CHECK(*model.reasoning == true);
    REQUIRE(model.input.has_value());
    REQUIRE(model.input->size() == 1);
    CHECK(model.input->front() == ai::ModelInput::Text);
    CHECK(*model.context_window == 131072);
    CHECK(*model.max_tokens == 32768);
    REQUIRE(model.cost.has_value());
    CHECK(model.cost->input == 2.0);
    CHECK(model.cost->output == 4.0);
    CHECK(model.cost->cache_read == 1.0);
    CHECK(model.cost->cache_write == 3.0);

    REQUIRE(provider->model_overrides.has_value());
    const auto override = provider->model_overrides->find("deepseek-v4-flash");
    REQUIRE(override != provider->model_overrides->end());
    CHECK(*override->second.max_tokens == 65536);
}

TEST_CASE("ModelConfig parses thinking level maps preserving null-unsupported semantics", "[coding_agent][model-config][issue345]") {
    tests::TempWorkspace workspace;
    auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "models": [{
            "id": "deepseek-v4-flash",
            "thinkingLevelMap": {"off": null, "high": "max"}
          }]
        }
      }
    })");
    const auto provider = config.provider("deepseek");
    REQUIRE(provider.has_value());
    REQUIRE(provider->models.has_value());
    const auto& map = provider->models->front().thinking_level_map;
    REQUIRE(map.has_value());
    const auto off = map->find(ai::ModelThinkingLevel::Off);
    REQUIRE(off != map->end());
    CHECK_FALSE(off->second.has_value());
    const auto high = map->find(ai::ModelThinkingLevel::High);
    REQUIRE(high != map->end());
    REQUIRE(high->second.has_value());
    CHECK(*high->second == "max");
}

TEST_CASE("ModelConfig unknown provider fields are ignored (no compat surface)", "[coding_agent][model-config][issue345]") {
    tests::TempWorkspace workspace;
    auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "compat": {"supportsStore": true},
          "oauth": "radius",
          "models": [{"id": "deepseek-v4-flash", "compat": {"unknown": 1}}]
        }
      }
    })");
    CHECK_FALSE(config.error().has_value());
    const auto provider = config.provider("deepseek");
    REQUIRE(provider.has_value());
    // The C++ config schema has no compat surface; the fields are ignored.
    REQUIRE(provider->models.has_value());
    CHECK(provider->models->front().id == "deepseek-v4-flash");
}
