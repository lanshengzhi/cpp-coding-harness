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

TEST_CASE("ProviderRequest resolution uses fake provider default model", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    const cch::coding_agent::UserSettings config;

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "fake",
        request,
        config,
        std::nullopt,
        std::nullopt);

    CHECK(resolved.model == "fake-model");
}

TEST_CASE("ProviderRequest resolution keeps settings provider identity on the OpenAI-compatible adapter", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    cch::coding_agent::UserSettings config;
    config.provider = "kimi-coding";
    config.model = "kimi-for-coding";
    config.base_url = "https://api.kimi.com/coding/v1";

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

TEST_CASE("ProviderRequest resolution uses the settings api_key_env chain when explicit is absent", "[settings][resolution]") {
    cch::coding_agent::ProviderRequest request;
    cch::coding_agent::UserSettings config;
    config.api_key_env = std::vector<std::string>{"FIRST", "SECOND"};

    const auto resolved = cch::coding_agent::resolve_provider_settings(
        "openai-compatible",
        request,
        config,
        std::nullopt,
        std::nullopt);

    REQUIRE(resolved.api_key_env_chain.size() == 2);
    CHECK(resolved.api_key_env_chain[0] == "FIRST");
    CHECK(resolved.api_key_env_chain[1] == "SECOND");
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
