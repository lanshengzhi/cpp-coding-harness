// RFC 9728 protected-resource discovery and RFC 8414 / OpenID Connect
// authorization-server discovery (spec #882, ticket #884). pi source at
// `7c10bd43` (v1.0.4): `packages/mcp/src/oauth/discovery.ts`. The transport is
// the scripted `OAuthHttpClient` seam — no live network — and the cases the
// checks would let through are built alongside the happy paths: the metadata
// must come from the retried bare-origin URL, not from the path-suffixed one
// that missed, and an issuer that does not match the authorization server URL
// must be rejected rather than accepted as metadata.

#include "ai/JsonAccess.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "coding_agent/mcp/McpOAuthDiscovery.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/Json.hpp"
#include "support/RuntimeFixture.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace mcp = coding_agent::mcp;

namespace {

constexpr std::string_view kProtectedResourceDocument = R"({
  "resource": "https://mcp.example.com/mcp",
  "authorization_servers": ["https://auth.example.com"],
  "scopes_supported": ["mcp.read"]
})";

[[nodiscard]] std::string authorization_server_document(
        std::string_view issuer, std::string_view registration_endpoint = "https://auth.example.com/register") {
    return std::string{"{\"issuer\":\""} + std::string{issuer} +
           "\",\"authorization_endpoint\":\"https://auth.example.com/authorize\","
           "\"token_endpoint\":\"https://auth.example.com/token\",\"response_types_supported\":[\"code\"],"
           "\"registration_endpoint\":\"" +
           std::string{registration_endpoint} + "\"}";
}

[[nodiscard]] support::JsonValue parse_json(std::string_view text) {
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

} // namespace

TEST_CASE("protected resource discovery retries the bare origin after a path-suffixed miss",
        "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    // The path-suffixed document is a 404 miss; only the bare-origin document
    // carries the authorization server, so a discovery that skipped the retry
    // could not produce this answer.
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource/mcp"] = {{404, "not found"}};
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource"] = {
            {200, std::string{kProtectedResourceDocument}}};

    tests::RuntimeFixture runtime;
    auto discovered = tests::run_awaitable(
            runtime, mcp::discover_protected_resource_metadata(http, "https://mcp.example.com/mcp"));
    REQUIRE(discovered.has_value());
    CHECK(discovered->resource == "https://mcp.example.com/mcp");
    REQUIRE(discovered->authorization_servers.has_value());
    CHECK(*discovered->authorization_servers == std::vector<std::string>{"https://auth.example.com"});
    REQUIRE(discovered->scopes_supported.has_value());
    CHECK(*discovered->scopes_supported == std::vector<std::string>{"mcp.read"});

    REQUIRE(http->get_requests.size() == 2);
    CHECK(http->get_requests[0].url == "https://mcp.example.com/.well-known/oauth-protected-resource/mcp");
    CHECK(http->get_requests[1].url == "https://mcp.example.com/.well-known/oauth-protected-resource");
    // pi's discovery headers.
    CHECK(http->get_requests[0].headers.at("Accept") == "application/json");
    CHECK(http->get_requests[0].headers.at("MCP-Protocol-Version") == std::string{mcp::kMcpDiscoveryProtocolVersion});
}

TEST_CASE("a root-path server does not retry a missed protected resource document",
        "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource"] = {
            {200, std::string{kProtectedResourceDocument}}};

    tests::RuntimeFixture runtime;
    auto discovered =
            tests::run_awaitable(runtime, mcp::discover_protected_resource_metadata(http, "https://mcp.example.com"));
    REQUIRE(discovered.has_value());
    // One request: a root-path server has no second candidate to try.
    CHECK(http->get_requests.size() == 1);
}

TEST_CASE("a protected resource document that is not JSON is an explicit failure",
        "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource/mcp"] = {{200, "not json"}};

    tests::RuntimeFixture runtime;
    auto discovered = tests::run_awaitable(
            runtime, mcp::discover_protected_resource_metadata(http, "https://mcp.example.com/mcp"));
    REQUIRE_FALSE(discovered.has_value());
    CHECK(discovered.error().code == support::ErrorCode::Validation);
    CHECK(discovered.error().message.find("Invalid OAuth protected resource metadata") != std::string::npos);
}

TEST_CASE("protected resource metadata rejects a missing resource URL", "[coding_agent][mcp][issue884][spec]") {
    auto missing = mcp::parse_protected_resource_metadata(parse_json(R"({"authorization_servers":[]})"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message == "Invalid OAuth protected resource metadata resource");

    auto bad_scheme =
            mcp::parse_protected_resource_metadata(parse_json(R"json({"resource":"javascript:alert(1)"})json"));
    REQUIRE_FALSE(bad_scheme.has_value());
    CHECK(bad_scheme.error().message == "Invalid OAuth protected resource metadata resource");

    auto bad_servers = mcp::parse_protected_resource_metadata(
            parse_json(R"({"resource":"https://mcp.example.com","authorization_servers":["not a url"]})"));
    REQUIRE_FALSE(bad_servers.has_value());
    CHECK(bad_servers.error().message == "Invalid authorization server URL");
}

TEST_CASE("authorization server discovery follows pi's URL order", "[coding_agent][mcp][issue884][spec]") {
    const std::vector<std::string> expected = {
            "https://auth.example.com/.well-known/oauth-authorization-server",
            "https://auth.example.com/.well-known/openid-configuration",
    };
    CHECK(mcp::authorization_server_discovery_urls("https://auth.example.com") == expected);
    // A trailing slash is not a path, so it produces the same candidates.
    CHECK(mcp::authorization_server_discovery_urls("https://auth.example.com/") == expected);
    // A non-root issuer path appends the path and adds the path-prefixed form.
    CHECK(mcp::authorization_server_discovery_urls("https://auth.example.com/tenant") ==
            std::vector<std::string>{
                    "https://auth.example.com/.well-known/oauth-authorization-server/tenant",
                    "https://auth.example.com/.well-known/openid-configuration/tenant",
                    "https://auth.example.com/tenant/.well-known/openid-configuration",
            });
}

TEST_CASE("authorization server discovery tries every candidate URL in order", "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses["https://auth.example.com/.well-known/oauth-authorization-server/tenant"] = {{404, "{}"}};
    http->get_responses["https://auth.example.com/.well-known/openid-configuration/tenant"] = {{502, "bad gateway"}};
    http->get_responses["https://auth.example.com/tenant/.well-known/openid-configuration"] = {
            {200, authorization_server_document("https://auth.example.com/tenant")}};

    tests::RuntimeFixture runtime;
    auto discovered = tests::run_awaitable(
            runtime, mcp::discover_authorization_server_metadata(http, "https://auth.example.com/tenant"));
    REQUIRE(discovered.has_value());
    REQUIRE(discovered->has_value());
    CHECK((*discovered)->issuer == "https://auth.example.com/tenant");
    REQUIRE((*discovered)->registration_endpoint.has_value());
    CHECK(*(*discovered)->registration_endpoint == "https://auth.example.com/register");

    REQUIRE(http->get_requests.size() == 3);
    CHECK(http->get_requests[0].url == "https://auth.example.com/.well-known/oauth-authorization-server/tenant");
    CHECK(http->get_requests[1].url == "https://auth.example.com/.well-known/openid-configuration/tenant");
    CHECK(http->get_requests[2].url == "https://auth.example.com/tenant/.well-known/openid-configuration");
}

TEST_CASE("authorization server metadata with a mismatched issuer is rejected", "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;

    // A bare origin and a trailing-slash issuer are the same issuer, so the
    // first candidate's metadata is accepted.
    auto same_issuer = std::make_shared<tests::FakeOAuthHttpClient>();
    same_issuer->get_responses["https://auth.example.com/.well-known/oauth-authorization-server"] = {
            {200, authorization_server_document("https://auth.example.com/")},
    };
    auto accepted = tests::run_awaitable(
            runtime, mcp::discover_authorization_server_metadata(same_issuer, "https://auth.example.com"));
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->has_value());
    CHECK((*accepted)->issuer == "https://auth.example.com/");

    // The first candidate misses and the next one names another issuer: the
    // document is rejected rather than accepted as this server's metadata.
    auto wrong_issuer = std::make_shared<tests::FakeOAuthHttpClient>();
    wrong_issuer->get_responses["https://auth.example.com/.well-known/oauth-authorization-server"] = {{404, "{}"}};
    wrong_issuer->get_responses["https://auth.example.com/.well-known/openid-configuration"] = {
            {200, authorization_server_document("https://attacker.example.com")},
    };
    auto mismatched = tests::run_awaitable(
            runtime, mcp::discover_authorization_server_metadata(wrong_issuer, "https://auth.example.com"));
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().code == support::ErrorCode::OAuth);
    CHECK(mismatched.error().message.find("OAuth issuer mismatch") != std::string::npos);

    // The explicit opt-out (configured metadata documents) accepts it.
    mcp::McpOAuthDiscoveryOptions skip;
    skip.skip_issuer_validation = true;
    auto skip_http = std::make_shared<tests::FakeOAuthHttpClient>();
    skip_http->get_responses["https://auth.example.com/.well-known/oauth-authorization-server"] = {
            {200, authorization_server_document("https://attacker.example.com")},
    };
    auto skipped = tests::run_awaitable(
            runtime, mcp::discover_authorization_server_metadata(skip_http, "https://auth.example.com", skip));
    REQUIRE(skipped.has_value());
    REQUIRE(skipped->has_value());
    CHECK((*skipped)->issuer == "https://attacker.example.com");
}

TEST_CASE("discoverOAuthServerInfo takes the first advertised authorization server",
        "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource/mcp"] = {
            {200, std::string{kProtectedResourceDocument}}};
    http->get_responses["https://auth.example.com/.well-known/oauth-authorization-server"] = {
            {200, authorization_server_document("https://auth.example.com")}};

    tests::RuntimeFixture runtime;
    auto info = tests::run_awaitable(runtime, mcp::discover_oauth_server_info(http, "https://mcp.example.com/mcp"));
    REQUIRE(info.has_value());
    CHECK(info->authorization_server_url == "https://auth.example.com");
    REQUIRE(info->authorization_server_metadata.has_value());
    CHECK(info->authorization_server_metadata->token_endpoint == "https://auth.example.com/token");
    REQUIRE(info->resource_metadata.has_value());
    CHECK(info->resource_metadata->resource == "https://mcp.example.com/mcp");
}

TEST_CASE("a configured authorization server metadata URL is used instead of discovery",
        "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    // No protected-resource document: the server publishes none.
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource/mcp"] = {{404, "{}"}};
    http->get_responses["https://mcp.example.com/.well-known/oauth-protected-resource"] = {{404, "{}"}};
    http->get_responses["https://idp.example.com/tenant"] = {
            {200, authorization_server_document("https://idp.example.com/tenant")}};

    mcp::McpOAuthDiscoveryOptions options;
    options.authorization_server_metadata_url = "https://idp.example.com/tenant";
    tests::RuntimeFixture runtime;
    auto info = tests::run_awaitable(
            runtime, mcp::discover_oauth_server_info(http, "https://mcp.example.com/mcp", options));
    REQUIRE(info.has_value());
    // The metadata document's issuer is the authorization-server URL.
    CHECK(info->authorization_server_url == "https://idp.example.com/tenant");
    CHECK_FALSE(info->resource_metadata.has_value());
    REQUIRE(info->authorization_server_metadata.has_value());
    CHECK(info->authorization_server_metadata->authorization_endpoint == "https://auth.example.com/authorize");
}

TEST_CASE("a network failure in protected resource discovery is not swallowed", "[coding_agent][mcp][issue884][spec]") {
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    // No scripted response: the fake reports a transport failure.
    tests::RuntimeFixture runtime;
    auto info = tests::run_awaitable(runtime, mcp::discover_oauth_server_info(http, "https://mcp.example.com/mcp"));
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().code == support::ErrorCode::Network);
}

TEST_CASE("WWW-Authenticate parsing reads only bearer and dpop challenges", "[coding_agent][mcp][issue884][spec]") {
    const auto challenge = mcp::parse_www_authenticate(
            "Bearer error=\"insufficient_scope\", scope=\"mcp.read mcp.write\", "
            "resource_metadata=\"https://mcp.example.com/.well-known/oauth-protected-resource/mcp\"");
    REQUIRE(challenge.error.has_value());
    CHECK(*challenge.error == "insufficient_scope");
    REQUIRE(challenge.scope.has_value());
    CHECK(*challenge.scope == "mcp.read mcp.write");
    REQUIRE(challenge.resource_metadata_url.has_value());
    CHECK(*challenge.resource_metadata_url == "https://mcp.example.com/.well-known/oauth-protected-resource/mcp");

    // A non-OAuth scheme carries no challenge fields, and an empty value counts
    // as absent rather than as an empty scope.
    const auto basic = mcp::parse_www_authenticate("Basic realm=\"x\", error=\"invalid_token\"");
    CHECK_FALSE(basic.error.has_value());
    const auto empty_scope = mcp::parse_www_authenticate("Bearer scope=\"\"");
    CHECK_FALSE(empty_scope.scope.has_value());

    const auto dpop = mcp::parse_www_authenticate("dpop error=invalid_token");
    REQUIRE(dpop.error.has_value());
    CHECK(*dpop.error == "invalid_token");
}

TEST_CASE("the OAuth resource parameter must match the MCP server", "[coding_agent][mcp][issue884][spec]") {
    auto matching = mcp::parse_protected_resource_metadata(parse_json(R"({
      "resource": "https://mcp.example.com",
      "authorization_servers": ["https://auth.example.com"]
    })"));
    REQUIRE(matching.has_value());
    auto selected = mcp::select_resource("https://mcp.example.com/mcp", std::optional{*matching});
    REQUIRE(selected.has_value());
    REQUIRE(selected->has_value());
    CHECK(**selected == "https://mcp.example.com");

    auto other_origin = mcp::parse_protected_resource_metadata(parse_json(R"({
      "resource": "https://other.example.com/mcp"
    })"));
    REQUIRE(other_origin.has_value());
    auto rejected = mcp::select_resource("https://mcp.example.com/mcp", std::optional{*other_origin});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::OAuth);
    CHECK(rejected.error().message == "Protected resource https://other.example.com/mcp does not match MCP server "
                                      "https://mcp.example.com/mcp");

    // No metadata means no resource parameter, not a guessed one.
    auto none = mcp::select_resource("https://mcp.example.com/mcp", std::nullopt);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());
}
