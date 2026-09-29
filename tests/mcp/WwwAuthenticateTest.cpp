#include "mcp/WwwAuthenticate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using cch::mcp::AuthorizationChallenge;
namespace wa = cch::mcp::www_authenticate;

} // namespace

TEST_CASE("a single Bearer challenge reduces to what the authorization flow reads", "[mcp][oauth]") {
    const auto challenge = wa::first_bearer_challenge(
            "Bearer resource_metadata=\"https://mcp.example.com/.well-known/oauth-protected-resource\", "
            "scope=\"mcp:tools read\"");
    REQUIRE(challenge.has_value());
    CHECK(challenge->scheme == "Bearer");
    CHECK(challenge->resource_metadata_url == "https://mcp.example.com/.well-known/oauth-protected-resource");
    CHECK(challenge->scope == "mcp:tools read");
    CHECK(challenge->error_codes.empty());
}

TEST_CASE("a quoted comma inside a parameter is not a challenge boundary", "[mcp][oauth]") {
    const auto challenges = wa::parse(
            "Bearer error_description=\"one, two, three\", scope=\"a b\", resource_metadata=\"https://x/prm\"");
    REQUIRE(challenges.size() == 1);
    CHECK(challenges.front().valid);
    const auto reduced = wa::reduce(challenges.front());
    CHECK(reduced.scope == "a b");
    CHECK(reduced.resource_metadata_url == "https://x/prm");
}

TEST_CASE("an escaped quote inside a quoted-string is part of the value", "[mcp][oauth]") {
    const auto challenges = wa::parse("Bearer error_description=\"say \\\"hello\\\", then stop\"");
    REQUIRE(challenges.size() == 1);
    CHECK(challenges.front().params.size() == 1);
    CHECK(challenges.front().params.front().second == "say \"hello\", then stop");
}

TEST_CASE("several challenges in one field value are separated by commas", "[mcp][oauth]") {
    const auto challenges = wa::parse(
            "Basic realm=\"upstream\", Bearer realm=\"mcp\", resource_metadata=\"https://x/prm\", scope=\"mcp:tools\"");
    REQUIRE(challenges.size() == 2);
    CHECK(challenges[0].scheme == "Basic");
    CHECK(challenges[0].params.size() == 1);
    CHECK(challenges[1].scheme == "Bearer");
    CHECK(challenges[1].params.size() == 3);
    const auto reduced = wa::reduce(challenges[1]);
    CHECK(reduced.resource_metadata_url == "https://x/prm");
    CHECK(reduced.scope == "mcp:tools");
}

TEST_CASE("a token68 credential is not mistaken for a parameter", "[mcp][oauth]") {
    const auto challenges = wa::parse("Basic dGVzdDp0ZXN0, Bearer realm=\"mcp\"");
    REQUIRE(challenges.size() == 2);
    CHECK(challenges[0].token68 == "dGVzdDp0ZXN0");
    CHECK(challenges[0].params.empty());
    CHECK(challenges[1].scheme == "Bearer");
    CHECK(challenges[1].params.size() == 1);
}

TEST_CASE("a scheme with no credential and no parameter is still a challenge", "[mcp][oauth]") {
    const auto challenges = wa::parse("Bearer");
    REQUIRE(challenges.size() == 1);
    CHECK(challenges.front().valid);
    CHECK(challenges.front().scheme == "Bearer");
    CHECK(challenges.front().params.empty());
    CHECK(challenges.front().token68.empty());
}

TEST_CASE("the scheme and the parameter names are matched case-insensitively", "[mcp][oauth]") {
    const auto challenge = wa::first_bearer_challenge("bearer ReSoUrCe_MeTaDaTa=\"https://x/prm\", SCOPE=\"mcp\"");
    REQUIRE(challenge.has_value());
    CHECK(challenge->scheme == "bearer");
    CHECK(challenge->resource_metadata_url == "https://x/prm");
    CHECK(challenge->scope == "mcp");
}

TEST_CASE("only a Bearer challenge reduces; another scheme is left for someone else to answer", "[mcp][oauth]") {
    CHECK_FALSE(wa::first_bearer_challenge("Basic realm=\"upstream\"").has_value());
    CHECK_FALSE(wa::first_bearer_challenge("Negotiate").has_value());
    CHECK(wa::first_bearer_challenge("Digest realm=\"x\", Bearer realm=\"mcp\"").has_value());
}

TEST_CASE("a malformed field contributes the challenges that preceded it and no more", "[mcp][oauth]") {
    // An unterminated quoted-string makes the rest of the field unreadable, and
    // a half-read authorization requirement is not actionable.
    const auto unterminated = wa::parse("Digest realm=\"x\", Bearer scope=\"unterminated");
    REQUIRE(unterminated.size() == 1);
    CHECK(unterminated.front().scheme == "Digest");

    CHECK(wa::parse("=oops").empty());
    CHECK(wa::parse("").empty());
    CHECK(wa::parse("   ").empty());
    CHECK_FALSE(wa::first_bearer_challenge("Bearer scope=\"unterminated").has_value());
}

TEST_CASE("a repeated error parameter contributes every code", "[mcp][oauth]") {
    const auto challenge = wa::first_bearer_challenge(
            "Bearer error=\"invalid_token\", error=\"insufficient_scope\", scope=\"mcp:tools\"");
    REQUIRE(challenge.has_value());
    REQUIRE(challenge->error_codes.size() == 2);
    CHECK(challenge->error_codes[0] == "invalid_token");
    CHECK(challenge->error_codes[1] == "insufficient_scope");
}

TEST_CASE("an empty parameter value is carried as an empty value, never invented", "[mcp][oauth]") {
    const auto challenge = wa::first_bearer_challenge("Bearer resource_metadata=\"\", error=\"\"");
    REQUIRE(challenge.has_value());
    CHECK(challenge->resource_metadata_url.empty());
    CHECK(challenge->error_codes.empty());
}

TEST_CASE("an auth-param with no value does not swallow the next challenge", "[mcp][oauth]") {
    const auto challenges = wa::parse("Bearer realm, Basic realm=\"upstream\"");
    REQUIRE(challenges.size() == 2);
    CHECK(challenges[0].scheme == "Bearer");
    CHECK(challenges[1].scheme == "Basic");
}
