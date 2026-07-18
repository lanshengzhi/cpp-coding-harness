#include "../../third_party/catch2/catch_test_macros.hpp"

#include "cch/coding_agent/Settings.hpp"

#include "coding_agent/ProviderConfigResolution.hpp"

#include <cstdlib>
#include <string>

namespace {

cch::coding_agent::UserSettings config_with_model(std::string model) {
    cch::coding_agent::UserSettings config;
    config.model = std::move(model);
    return config;
}

} // namespace

TEST_CASE("resolve_provider_settings prefers explicit CLI model over user settings", "[settings][resolution]") {
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

TEST_CASE("resolve_provider_settings uses user settings model when CLI omits model", "[settings][resolution]") {
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

TEST_CASE("resolve_provider_settings uses stored model on resume when CLI omits model", "[settings][resolution]") {
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

TEST_CASE("resolve_provider_settings lets explicit CLI model override stored model", "[settings][resolution]") {
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

TEST_CASE("resolve_provider_settings falls back to provider default model", "[settings][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli;
    const cch::coding_agent::UserSettings config;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        false,
        cli,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "gpt-4.1-mini");
}

TEST_CASE("resolve_provider_settings uses fake provider default model", "[settings][resolution]") {
    const cch::coding_agent::CliProviderOverrides cli;
    const cch::coding_agent::UserSettings config;
    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "fake",
        true,
        cli,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "fake-model");
}

TEST_CASE("resolve_provider_settings uses user settings base_url when CLI omits base_url", "[settings][resolution]") {
    cch::coding_agent::UserSettings config;
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

TEST_CASE("resolve_provider_settings keeps custom provider identity on OpenAI-compatible adapter", "[settings][resolution]") {
    cch::coding_agent::UserSettings config;
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

TEST_CASE("resolve_provider_settings preserves stored provider identity on resume", "[settings][resolution]") {
    cch::coding_agent::UserSettings config;
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

TEST_CASE("resolved_api_key_env_chain prefers CLI override", "[settings][resolution]") {
    cch::coding_agent::UserSettings config;
    config.api_key_env = std::vector<std::string>{"FIRST", "SECOND"};
    const cch::coding_agent::CliProviderOverrides cli{
        .api_key_env = "CLI_KEY",
    };

    const auto chain = cch::coding_agent::resolved_api_key_env_chain(cli, config);
    REQUIRE(chain.size() == 1);
    CHECK(chain.front() == "CLI_KEY");
}

TEST_CASE("resolved_api_key_env_chain uses config chain when CLI omits api_key_env", "[settings][resolution]") {
    cch::coding_agent::UserSettings config;
    config.api_key_env = std::vector<std::string>{"FIRST", "SECOND"};
    const cch::coding_agent::CliProviderOverrides cli;

    const auto chain = cch::coding_agent::resolved_api_key_env_chain(cli, config);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == "FIRST");
    CHECK(chain[1] == "SECOND");
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared ProviderRequest precedence policy used by both CLI and SDK paths
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ProviderRequest resolution prefers explicit model over user settings", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    request.model = "sdk-explicit-model";

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config_with_model("config-model"),
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "sdk-explicit-model");
}

TEST_CASE("ProviderRequest resolution uses stored model on resume when explicit is absent", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config_with_model("config-model"),
        std::string{"stored-provider"},
        std::string{"stored-model"});

    CHECK(resolved.provider == "stored-provider");
    CHECK(resolved.model == "stored-model");
}

TEST_CASE("ProviderRequest resolution lets explicit model override stored model", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    request.model = "sdk-explicit-model";

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config_with_model("config-model"),
        std::string{"stored-provider"},
        std::string{"stored-model"});

    CHECK(resolved.model == "sdk-explicit-model");
}

TEST_CASE("ProviderRequest resolution uses user settings model when no explicit or stored model", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config_with_model("config-model"),
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "config-model");
}

TEST_CASE("ProviderRequest resolution falls back to provider default model", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    const cch::coding_agent::UserSettings config;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "gpt-4.1-mini");
}

TEST_CASE("ProviderRequest resolution preserves explicit provider identity", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    request.provider = "kimi-coding";
    request.model = "kimi-for-coding";
    request.base_url = "https://api.kimi.com/coding/v1";
    const cch::coding_agent::UserSettings config;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.provider_registry_name == "openai-compatible");
    CHECK(resolved.provider == "kimi-coding");
    CHECK(resolved.api == "openai-completions");
    CHECK(resolved.model == "kimi-for-coding");
    CHECK(resolved.base_url == "https://api.kimi.com/coding/v1");
}

TEST_CASE("ProviderRequest resolution preserves stored provider and model on resume", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    cch::coding_agent::UserSettings config;
    config.provider = "openai-compatible";

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config,
        std::string{"kimi-coding"},
        std::string{"kimi-for-coding"});

    CHECK(resolved.provider_registry_name == "openai-compatible");
    CHECK(resolved.provider == "kimi-coding");
    CHECK(resolved.api == "openai-completions");
    CHECK(resolved.model == "kimi-for-coding");
}

TEST_CASE("ProviderRequest resolution explicit api_key_env overrides config chain", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    request.api_key_env = std::vector<std::string>{"SDK_KEY"};

    cch::coding_agent::UserSettings config;
    config.api_key_env = std::vector<std::string>{"FIRST", "SECOND"};

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config,
        std::nullopt,
        std::nullopt);

    REQUIRE(resolved.api_key_env_chain.size() == 1);
    CHECK(resolved.api_key_env_chain.front() == "SDK_KEY");
}

TEST_CASE("ProviderRequest resolution uses config provider for all registry names", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    cch::coding_agent::UserSettings config;
    config.provider = "kimi-coding";

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "sdk-host",
        request,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.provider_registry_name == "sdk-host");
    CHECK(resolved.provider == "kimi-coding");
}

TEST_CASE("ProviderRequest resolution explicit provider wins over settings provider", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    request.provider = "openai";
    cch::coding_agent::UserSettings config;
    config.provider = "kimi-coding";

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "sdk-host",
        request,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.provider == "openai");
}

TEST_CASE("ProviderRequest resolution stored provider wins over settings provider", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    cch::coding_agent::UserSettings config;
    config.provider = "kimi-coding";

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config,
        std::string{"stored-provider"},
        std::string{"stored-model"});

    CHECK(resolved.provider == "stored-provider");
}
