#include <cch/ai/Models.hpp>
#include "coding_agent/ModelConfig.hpp"
#include "coding_agent/ProviderComposer.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"
#include "support/ReadyResult.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

class ComposerAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> environment(std::string name) const override {
        const auto found = environment_values.find(name);
        if (found == environment_values.end()) {
            return tests::ready_result<std::optional<std::string>>(std::nullopt);
        }
        return tests::ready_result<std::optional<std::string>>(found->second);
    }

    [[nodiscard]] support::AsyncResult<bool> file_exists(std::string) const override {
        return tests::ready_result<bool>(false);
    }

    std::map<std::string, std::string, std::less<>> environment_values;
};

template <typename T> [[nodiscard]] support::Expected<T> consume_async_result(support::AsyncResult<T> operation) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
            io,
            [](support::AsyncResult<T> value) -> boost::asio::awaitable<support::Expected<T>> {
                co_return co_await support::detail::await_async_result(std::move(value));
            }(std::move(operation)),
            boost::asio::use_future);
    io.run();
    return future.get();
}

[[nodiscard]] ai::ProviderDefinition composer_auth_definition(ai::ProviderAuth auth) {
    return ai::ProviderDefinition{
            .id = "fixture-provider",
            .name = "Fixture Provider",
            .models = {tests::make_model("fixture-model", "fixture-provider", "fixture-api")},
            .auth = std::move(auth),
    };
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
/// the config path.
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

TEST_CASE("builtin definitions carry the Codex 6 and Kimi 4 catalogs",
        "[coding_agent][provider-composer][issue546][spec]") {
    const auto builtins = ai::builtin_provider_definitions();
    REQUIRE(builtins.size() == 6);

    const auto codex_it =
            std::find_if(builtins.begin(), builtins.end(), [](const auto& b) { return b.id == "openai-codex"; });
    REQUIRE(codex_it != builtins.end());
    const auto& codex = *codex_it;
    CHECK(codex.id == "openai-codex");
    CHECK(codex.name == "OpenAI Codex");
    CHECK(codex.auth.oauth.has_value());
    CHECK_FALSE(codex.auth.api_key.has_value());
    REQUIRE(codex.models.size() == 6);
    CHECK(codex.models.front().id == "gpt-5.3-codex-spark");
    CHECK(codex.models.back().id == "gpt-6-astra");
    const auto gpt55 = std::find_if(
            codex.models.begin(), codex.models.end(), [](const ai::Model& m) { return m.id == "gpt-5.5"; });
    REQUIRE(gpt55 != codex.models.end());
    CHECK(gpt55->api == "openai-codex-responses");
    CHECK(gpt55->base_url == "https://chatgpt.com/backend-api");

    const auto kimi_it =
            std::find_if(builtins.begin(), builtins.end(), [](const auto& b) { return b.id == "kimi-coding"; });
    REQUIRE(kimi_it != builtins.end());
    const auto& kimi = *kimi_it;
    CHECK(kimi.id == "kimi-coding");
    CHECK(kimi.name == "Kimi For Coding");
    CHECK(kimi.auth.api_key.has_value());
    CHECK_FALSE(kimi.auth.oauth.has_value());
    REQUIRE(kimi.models.size() == 4);
    const auto kimi_coding = std::find_if(
            kimi.models.begin(), kimi.models.end(), [](const ai::Model& m) { return m.id == "kimi-for-coding"; });
    REQUIRE(kimi_coding != kimi.models.end());
    CHECK(kimi_coding->api == "openai-completions");
    CHECK(kimi_coding->base_url == "https://api.kimi.com/coding/v1");
    CHECK_FALSE(kimi_coding->headers.has_value());
}

TEST_CASE("builtin catalogs match the hash-pinned snapshot shard values",
        "[coding_agent][provider-composer][issue370][spec]") {
    // The Codex shard is the current hash-pinned T0 parity artifact. Kimi
    // deliberately diverges from its upstream shard and uses the
    // vendor-authoritative catalog instead. The shard compat members are
    // excluded (see
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
    REQUIRE(builtins.size() == 6);
    const auto codex_it =
            std::find_if(builtins.begin(), builtins.end(), [](const auto& b) { return b.id == "openai-codex"; });
    const auto kimi_it =
            std::find_if(builtins.begin(), builtins.end(), [](const auto& b) { return b.id == "kimi-coding"; });
    REQUIRE(codex_it != builtins.end());
    REQUIRE(kimi_it != builtins.end());
    check_shard(codex_it->models, "models/providers/openai-codex.json", "openai-codex-responses");
    check_shard(kimi_it->models, "models/vendors/kimi-coding.json", "openai-completions");
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
    const auto composed = compose_fixture_models(workspace, "anthropic", {*fixture});
    REQUIRE(composed.size() == 1);

    auto expected = tests::make_model("claude-complete", "anthropic", "anthropic-messages");
    expected.name = "Claude Complete";
    expected.base_url = "https://api.anthropic.com";
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
    CHECK(change.definition->models.size() == 6);
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
    const auto gpt56_luna =
            std::find_if(models.begin(), models.end(), [](const ai::Model& m) { return m.id == "gpt-5.6-luna"; });
    REQUIRE(gpt56_luna != models.end());
    CHECK(gpt56_luna->base_url == "https://codex.example/v1");
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

TEST_CASE("a config-only provider with an unknown api still composes its model",
        "[coding_agent][provider-composer][issue671][spec]") {
    tests::TempWorkspace workspace;
    // The unknown api is a parse warning, never a rejection: the provider and
    // its model reach the catalog with the api intact, and only the stream
    // dispatch reports "has no API implementation" (#671).
    const auto config = load_models_json(workspace, R"({
      "providers": {
        "deepseek": {
          "name": "DeepSeek",
          "baseUrl": "https://api.deepseek.example/v1",
          "api": "openai-responses",
          "apiKey": "dummy-deepseek-key",
          "models": [{"id": "deepseek-v4-flash", "api": "made-up-api"}]
        }
      }
    })");
    REQUIRE(config.warnings().size() == 1);
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("deepseek", std::nullopt, config, composer_options(), error);
    CHECK_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    const auto& models = change.definition->models;
    REQUIRE(models.size() == 1);
    CHECK(models.front().id == "deepseek-v4-flash");
    CHECK(models.front().api == "made-up-api");
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
    CHECK_FALSE(change.definition->auth.oauth.has_value());
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

TEST_CASE("composed auth callbacks preserve inherited hooks and stored credentials",
        "[coding_agent][provider-composer][auth][issue546][spec]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {"fixture-provider": {"baseUrl": "https://fixture.example"}}
    })");

    int check_calls = 0;
    int resolve_calls = 0;
    ai::ApiKeyAuth inherited;
    inherited.name = "Inherited key";
    inherited.check =
            [&check_calls](const ai::AuthContext&,
                    std::optional<ai::ApiKeyCredential>) -> support::AsyncResult<std::optional<ai::AuthCheck>> {
        ++check_calls;
        return tests::ready_result<std::optional<ai::AuthCheck>>(
                ai::AuthCheck{.source = "inherited check", .type = ai::AuthType::ApiKey});
    };
    inherited.resolve = [&resolve_calls](const ai::AuthContext&, std::optional<ai::ApiKeyCredential> credential)
            -> support::AsyncResult<std::optional<ai::AuthResult>> {
        ++resolve_calls;
        ai::AuthResult result{
                .auth = ai::ModelAuth{.api_key = credential && credential->key ? *credential->key : "ambient-key"},
                .source = "inherited resolve",
        };
        return tests::ready_result<std::optional<ai::AuthResult>>(std::move(result));
    };

    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("fixture-provider",
            composer_auth_definition(ai::ProviderAuth{.api_key = std::move(inherited)}),
            config,
            composer_options(),
            error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    const ComposerAuthContext context;
    auto& auth = *change.definition->auth.api_key;

    auto checked = consume_async_result(auth.check(context, ai::ApiKeyCredential{.key = "stored-key"}));
    REQUIRE(checked.has_value());
    REQUIRE(checked->has_value());
    CHECK(checked->value().source == "inherited check");

    auto resolved = consume_async_result(auth.resolve(context, ai::ApiKeyCredential{.key = "stored-key"}));
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->has_value());
    CHECK(resolved->value().auth.api_key == std::optional<std::string>{"stored-key"});
    CHECK(resolved->value().source == "inherited resolve");

    auto ambient_check = consume_async_result(auth.check(context, std::nullopt));
    REQUIRE(ambient_check.has_value());
    REQUIRE(ambient_check->has_value());
    CHECK(ambient_check->value().source == "inherited check");
    auto ambient_resolve = consume_async_result(auth.resolve(context, std::nullopt));
    REQUIRE(ambient_resolve.has_value());
    REQUIRE(ambient_resolve->has_value());
    CHECK(ambient_resolve->value().auth.api_key == std::optional<std::string>{"ambient-key"});
    CHECK(check_calls == 2);
    CHECK(resolve_calls == 2);
}

TEST_CASE("composed auth callbacks select configured keys and retain no-key behavior",
        "[coding_agent][provider-composer][auth][issue546][spec]") {
    tests::TempWorkspace workspace;
    const auto configured = load_models_json(workspace, R"({
      "providers": {"fixture-provider": {
        "baseUrl": "https://fixture.example", "apiKey": "configured-key"
      }}
    })");
    std::optional<std::string> error;
    auto configured_change = coding_agent::compose_provider("fixture-provider",
            composer_auth_definition(ai::ProviderAuth{.api_key = ai::ApiKeyAuth{.name = "key"}}),
            configured,
            composer_options(),
            error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(configured_change.definition.has_value());
    const ComposerAuthContext context;
    auto& configured_auth = *configured_change.definition->auth.api_key;

    auto checked = consume_async_result(configured_auth.check(context, std::nullopt));
    REQUIRE(checked.has_value());
    REQUIRE(checked->has_value());
    CHECK(checked->value().source == "configured API key");
    auto resolved = consume_async_result(configured_auth.resolve(context, std::nullopt));
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->has_value());
    CHECK(resolved->value().auth.api_key == std::optional<std::string>{"configured-key"});
    CHECK(resolved->value().source == "configured API key");
    auto stored_check = consume_async_result(configured_auth.check(context, ai::ApiKeyCredential{.key = "stored-key"}));
    REQUIRE(stored_check.has_value());
    REQUIRE(stored_check->has_value());
    CHECK(stored_check->value().source == "stored credential");

    auto stored = consume_async_result(configured_auth.resolve(context, ai::ApiKeyCredential{.key = "stored-key"}));
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    CHECK(stored->value().auth.api_key == std::optional<std::string>{"stored-key"});
    CHECK(stored->value().source == "stored credential");

    const auto no_key = load_models_json(workspace, R"({
      "providers": {"fixture-provider": {"baseUrl": "https://fixture.example"}}
    })");
    error.reset();
    auto no_key_change = coding_agent::compose_provider("fixture-provider",
            composer_auth_definition(ai::ProviderAuth{.api_key = ai::ApiKeyAuth{.name = "key"}}),
            no_key,
            composer_options(),
            error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(no_key_change.definition.has_value());
    auto& no_key_auth = *no_key_change.definition->auth.api_key;
    auto no_key_check = consume_async_result(no_key_auth.check(context, std::nullopt));
    REQUIRE(no_key_check.has_value());
    CHECK_FALSE(no_key_check->has_value());
    auto no_key_resolve = consume_async_result(no_key_auth.resolve(context, std::nullopt));
    REQUIRE(no_key_resolve.has_value());
    CHECK_FALSE(no_key_resolve->has_value());
}

TEST_CASE("configured auth headers are resolved with the composed key",
        "[coding_agent][provider-composer][auth][issue546][spec]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {"fixture-provider": {
        "baseUrl": "https://fixture.example",
        "apiKey": "configured-key",
        "headers": {"X-Static": "fixed", "X-Token": "$HEADER_TOKEN"}
      }}
    })");
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("fixture-provider",
            composer_auth_definition(ai::ProviderAuth{.api_key = ai::ApiKeyAuth{.name = "key"}}),
            config,
            composer_options(),
            error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    ComposerAuthContext context;
    context.environment_values.emplace("HEADER_TOKEN", "ambient-header");
    auto result = consume_async_result(change.definition->auth.api_key->resolve(context, std::nullopt));
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(result->value().auth.api_key == std::optional<std::string>{"configured-key"});
    CHECK(result->value().auth.headers.at("X-Static") == "fixed");
    CHECK(result->value().auth.headers.at("X-Token") == "ambient-header");
}

TEST_CASE("composed auth callbacks propagate inherited failures",
        "[coding_agent][provider-composer][auth][issue546][spec]") {
    tests::TempWorkspace workspace;
    const auto config = load_models_json(workspace, R"({
      "providers": {"fixture-provider": {"baseUrl": "https://fixture.example"}}
    })");
    ai::ApiKeyAuth failing;
    failing.name = "failing key";
    failing.check = [](const ai::AuthContext&,
                            std::optional<ai::ApiKeyCredential>) -> support::AsyncResult<std::optional<ai::AuthCheck>> {
        return tests::failed_result<std::optional<ai::AuthCheck>>(
                support::make_error(support::ErrorCode::Auth, "inherited check failed"));
    };
    failing.resolve =
            [](const ai::AuthContext&,
                    std::optional<ai::ApiKeyCredential>) -> support::AsyncResult<std::optional<ai::AuthResult>> {
        return tests::failed_result<std::optional<ai::AuthResult>>(
                support::make_error(support::ErrorCode::Auth, "inherited resolve failed"));
    };
    std::optional<std::string> error;
    auto change = coding_agent::compose_provider("fixture-provider",
            composer_auth_definition(ai::ProviderAuth{.api_key = std::move(failing)}),
            config,
            composer_options(),
            error);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(change.definition.has_value());
    const ComposerAuthContext context;
    auto check = consume_async_result(change.definition->auth.api_key->check(context, std::nullopt));
    REQUIRE_FALSE(check.has_value());
    CHECK(check.error().code == support::ErrorCode::Auth);
    CHECK(check.error().message == "inherited check failed");
    auto resolve = consume_async_result(change.definition->auth.api_key->resolve(context, std::nullopt));
    REQUIRE_FALSE(resolve.has_value());
    CHECK(resolve.error().code == support::ErrorCode::Auth);
    CHECK(resolve.error().message == "inherited resolve failed");
}

TEST_CASE(
        "default-model table maps the supported provider subset", "[coding_agent][provider-composer][issue345][spec]") {
    CHECK(coding_agent::default_model_for_provider("openai-codex") == std::optional<std::string>{"gpt-5.5"});
    CHECK(coding_agent::default_model_for_provider("kimi-coding") == std::optional<std::string>{"kimi-for-coding"});
    // Config-only deepseek has no default model (the "no default model" branch).
    CHECK_FALSE(coding_agent::default_model_for_provider("deepseek").has_value());
    CHECK_FALSE(coding_agent::default_model_for_provider("unknown-provider").has_value());
}
