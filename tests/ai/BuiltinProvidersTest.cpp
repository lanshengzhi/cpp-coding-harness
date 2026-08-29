#include <cch/ai/Models.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

using namespace cch;

TEST_CASE(
    "Built-in provider definitions preserve the frozen catalogs and auth methods",
    "[ai][providers][issue545]") {
    static_assert(std::movable<ai::ProviderDefinition>);
    static_assert(!std::copy_constructible<ai::ProviderDefinition>);

    auto definitions = ai::builtin_provider_definitions();

    REQUIRE(definitions.size() == 2);

    REQUIRE(definitions[0].id == "openai-codex");
    CHECK(definitions[0].name == "OpenAI Codex");
    REQUIRE(definitions[0].models.size() == 7);
    CHECK(definitions[0].models.front().id == "gpt-5.3-codex-spark");
    CHECK(definitions[0].models.back().id == "gpt-5.6-terra");
    REQUIRE(definitions[0].auth.oauth);
    CHECK(definitions[0].auth.oauth->name == "OpenAI (ChatGPT Plus/Pro)");
    CHECK(static_cast<bool>(definitions[0].auth.oauth->login));
    CHECK(static_cast<bool>(definitions[0].auth.oauth->refresh));
    CHECK(static_cast<bool>(definitions[0].auth.oauth->to_auth));
    CHECK_FALSE(definitions[0].auth.api_key);

    REQUIRE(definitions[1].id == "kimi-coding");
    CHECK(definitions[1].name == "Kimi For Coding");
    REQUIRE(definitions[1].models.size() == 4);
    CHECK(definitions[1].models.front().id == "k3");
    CHECK(definitions[1].models.back().id == "kimi-for-coding-highspeed");
    REQUIRE(definitions[1].auth.api_key);
    CHECK(definitions[1].auth.api_key->name == "Kimi API key");
    CHECK(static_cast<bool>(definitions[1].auth.api_key->check));
    CHECK(static_cast<bool>(definitions[1].auth.api_key->resolve));
    CHECK_FALSE(static_cast<bool>(definitions[1].auth.api_key->login));
    REQUIRE(definitions[1].auth.oauth);
    CHECK(definitions[1].auth.oauth->name == "Kimi Code (subscription)");
    CHECK(static_cast<bool>(definitions[1].auth.oauth->login));
    CHECK(static_cast<bool>(definitions[1].auth.oauth->refresh));
    CHECK(static_cast<bool>(definitions[1].auth.oauth->to_auth));
}

TEST_CASE(
    "Built-in provider definitions are fresh on every call",
    "[ai][providers][issue545]") {
    auto first = ai::builtin_provider_definitions();
    auto second = ai::builtin_provider_definitions();

    REQUIRE(first.size() == second.size());
    REQUIRE(first.size() == 2);
    REQUIRE(first[0].models.size() == second[0].models.size());
    REQUIRE(first[1].models.size() == second[1].models.size());

    first[0].name = "mutated";
    first[0].models.front().name = "mutated";
    first[0].auth.oauth->name = "mutated";
    first[1].models.front().name = "mutated";
    first[1].auth.api_key->name = "mutated";

    CHECK(second[0].name == "OpenAI Codex");
    CHECK(second[0].models.front().name == "GPT-5.3 Codex Spark");
    CHECK(second[0].auth.oauth->name == "OpenAI (ChatGPT Plus/Pro)");
    CHECK(second[1].models.front().name == "Kimi K3");
    CHECK(second[1].auth.api_key->name == "Kimi API key");
}
