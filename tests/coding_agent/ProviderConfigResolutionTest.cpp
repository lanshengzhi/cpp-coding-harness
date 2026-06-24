#include "../../third_party/catch2/catch_test_macros.hpp"

#include "cch/coding_agent/Config.hpp"

#include <cstdlib>
#include <string>

namespace {

cch::coding_agent::ConfigData config_with_model(std::string model) {
    cch::coding_agent::ConfigData config;
    config.model = std::move(model);
    return config;
}

} // namespace

TEST_CASE("resolve_provider_settings prefers explicit CLI model over config", "[config][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli{
        .model = "cli-model",
    };
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config_with_model("config-model"),
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "cli-model");
}

TEST_CASE("resolve_provider_settings uses config model when CLI omits model", "[config][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config_with_model("config-model"),
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "config-model");
}

TEST_CASE("resolve_provider_settings uses stored model on resume when CLI omits model", "[config][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config_with_model("config-model"),
        std::nullopt,
        std::string{"stored-model"});

    CHECK(resolved.model == "stored-model");
}

TEST_CASE("resolve_provider_settings lets explicit CLI model override stored model", "[config][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli{
        .model = "cli-model",
    };
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config_with_model("config-model"),
        std::nullopt,
        std::string{"stored-model"});

    CHECK(resolved.model == "cli-model");
}

TEST_CASE("resolve_provider_settings falls back to provider default model", "[config][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli;
    const cch::coding_agent::ConfigData config;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "gpt-4.1-mini");
}

TEST_CASE("resolve_provider_settings uses fake provider default model", "[config][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli;
    const cch::coding_agent::ConfigData config;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "fake",
        true,
        cli,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "fake-model");
}

TEST_CASE("resolve_provider_settings uses config base_url when CLI omits base_url", "[config][resolution]") {
    cch::coding_agent::ConfigData config;
    config.base_url = "https://config.example/v1";
    const cch::coding_agent::CliProviderOverrides cli;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.base_url == "https://config.example/v1");
}

TEST_CASE("resolve_provider_settings keeps custom provider identity on OpenAI-compatible adapter", "[config][resolution]") {
    cch::coding_agent::ConfigData config;
    config.provider = "kimi-coding";
    config.model = "kimi-for-coding";
    config.base_url = "https://api.kimi.com/coding/v1";
    const cch::coding_agent::CliProviderOverrides cli;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.provider_registry_name == "openai-compatible");
    CHECK(resolved.provider == "kimi-coding");
    CHECK(resolved.api == "openai-completions");
    CHECK(resolved.model == "kimi-for-coding");
    CHECK(resolved.base_url == "https://api.kimi.com/coding/v1");
}

TEST_CASE("resolve_provider_settings preserves stored provider identity on resume", "[config][resolution]") {
    cch::coding_agent::ConfigData config;
    config.provider = "openai-compatible";
    const cch::coding_agent::CliProviderOverrides cli;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config,
        std::string{"kimi-coding"},
        std::string{"kimi-for-coding"});

    CHECK(resolved.provider_registry_name == "openai-compatible");
    CHECK(resolved.provider == "kimi-coding");
    CHECK(resolved.api == "openai-completions");
    CHECK(resolved.model == "kimi-for-coding");
}

TEST_CASE("resolved_api_key_env_chain prefers CLI override", "[config][resolution]") {
    cch::coding_agent::ConfigData config;
    config.api_key_env = std::vector<std::string>{"FIRST", "SECOND"};
    const cch::coding_agent::CliProviderOverrides cli{
        .api_key_env = "CLI_KEY",
    };

    const auto chain = cch::coding_agent::resolved_api_key_env_chain(cli, config);
    REQUIRE(chain.size() == 1);
    CHECK(chain.front() == "CLI_KEY");
}

TEST_CASE("resolved_api_key_env_chain uses config chain when CLI omits api_key_env", "[config][resolution]") {
    cch::coding_agent::ConfigData config;
    config.api_key_env = std::vector<std::string>{"FIRST", "SECOND"};
    const cch::coding_agent::CliProviderOverrides cli;

    const auto chain = cch::coding_agent::resolved_api_key_env_chain(cli, config);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == "FIRST");
    CHECK(chain[1] == "SECOND");
}

TEST_CASE("ConfigLoader default_config_path uses HOME", "[config][resolution]") {
#if defined(__unix__) || defined(__APPLE__)
    const auto previous = std::getenv("HOME");
    setenv("HOME", "/tmp/test-home", 1);
    CHECK(cch::coding_agent::ConfigLoader::default_config_path() == "/tmp/test-home/.cpp-harness/config.json");
    if (previous != nullptr) {
        setenv("HOME", previous, 1);
    } else {
        unsetenv("HOME");
    }
#else
    SUCCEED("HOME-based default_config_path test skipped on this platform");
#endif
}
