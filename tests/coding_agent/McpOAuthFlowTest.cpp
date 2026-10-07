// The MCP OAuth authorization flow (spec #882, ticket #884): dynamic client
// registration, SEP-837 `application_type`, Client ID Metadata Documents, PKCE
// S256, the authorization-code and refresh grants, and pi's error semantics.
// pi source at `7c10bd43` (v1.0.4): `packages/mcp/src/oauth/flow.ts` and
// `packages/coding-agent/src/extensions/mcp/oauth.ts`. The authorization
// server is the scripted `OAuthHttpClient` seam — no live network — and the
// cases the checks would let through are built alongside the happy paths: a
// dead refresh token must invalidate the tokens and re-authorize instead of
// looping, and a code from another issuer must never reach the token endpoint.

#include "ai/JsonAccess.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/auth/Pkce.hpp"
#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthFlow.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/Json.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Timestamps.hpp>
#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace mcp = coding_agent::mcp;

namespace {

const std::string kServerUrl = "https://mcp.example.com/mcp";
const std::string kRedirectUrl = "http://127.0.0.1:41234/callback";
const std::string kResourceDocument =
        R"({"resource":"https://mcp.example.com/mcp","authorization_servers":["https://auth.example.com"],)" +
        std::string{R"("scopes_supported":["mcp.read","mcp.write"]})"};
const std::string kAuthorizationServerDocument =
        R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)" +
        std::string{R"("token_endpoint":"https://auth.example.com/token",)"} +
        std::string{R"("registration_endpoint":"https://auth.example.com/register",)"} +
        std::string{R"("response_types_supported":["code"],)"} +
        std::string{R"("code_challenge_methods_supported":["S256"],)"} +
        std::string{R"("token_endpoint_auth_methods_supported":["none"]})"};
const std::string kRegistrationResponse =
        R"({"client_id":"dyn-client-1","redirect_uris":["http://127.0.0.1:41234/callback"]})";
const std::string kTokenResponse = R"({"access_token":"access-1","token_type":"Bearer","expires_in":3600,)" +
                                   std::string{R"("refresh_token":"refresh-1","scope":"mcp.read"})"};

const std::string kResourceMetadataUrl = "https://mcp.example.com/.well-known/oauth-protected-resource/mcp";
const std::string kAuthorizationServerMetadataUrl = "https://auth.example.com/.well-known/oauth-authorization-server";
const std::string kRegistrationUrl = "https://auth.example.com/register";
const std::string kTokenUrl = "https://auth.example.com/token";

struct Harness {
    std::shared_ptr<tests::FakeOAuthHttpClient> http = std::make_shared<tests::FakeOAuthHttpClient>();
    tests::TempWorkspace workspace;
    std::shared_ptr<mcp::McpAuthStore> store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    mcp::McpOAuthConfig oauth;
    /// The discovery documents, re-scripted for every run because a flow run
    /// fetches them again (discovery is not cached in this slice).
    std::string resource_document{kResourceDocument};
    std::string authorization_server_document{kAuthorizationServerDocument};

    Harness() {
        http->responses[kRegistrationUrl] = {{200, kRegistrationResponse}};
        http->responses[kTokenUrl] = {{200, kTokenResponse}};
    }

    [[nodiscard]] support::Expected<mcp::McpOAuthFlowOutcome> run(
            tests::RuntimeFixture& runtime, mcp::McpOAuthFlowOptions options = {}) {
        // Every run starts from a fresh discovery script: one attempt discovers
        // once, and a retried attempt (pi's `invalid_client` / `invalid_grant`
        // rule) discovers again.
        http->get_responses[kResourceMetadataUrl].assign({{200, resource_document}, {200, resource_document}});
        http->get_responses[kAuthorizationServerMetadataUrl].assign(
                {{200, authorization_server_document}, {200, authorization_server_document}});
        return tests::run_awaitable(
                runtime, mcp::authorize_mcp(store, "echo", kServerUrl, oauth, kRedirectUrl, http, std::move(options)));
    }

    [[nodiscard]] std::optional<mcp::McpOAuthState> state() {
        auto stored = store->load("echo", kServerUrl);
        REQUIRE(stored.has_value());
        return std::move(*stored);
    }
};

[[nodiscard]] support::JsonValue parse_json(std::string_view text) {
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] std::string query_param(std::string_view url, std::string_view key) {
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return {};
    }
    const auto pairs = ai::auth::parse_query_pairs(url.substr(query_start + 1));
    const auto found = pairs.find(std::string{key});
    return found == pairs.end() ? std::string{} : found->second;
}

} // namespace

TEST_CASE("an oauth-configured server with no client id discovers, registers, and asks for authorization",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;

    auto outcome = harness.run(runtime);
    REQUIRE(outcome.has_value());
    CHECK(outcome->result == mcp::McpOAuthFlowResult::Redirect);
    REQUIRE(outcome->authorization_url.has_value());

    // The registration body is pi's: the redirect URI, the SEP-837
    // `application_type` for a loopback redirect, and the requested scope,
    // which defaults to the server's advertised scopes.
    REQUIRE(harness.http->requests.size() == 1);
    const auto& registration = harness.http->requests.front();
    CHECK(registration.url == kRegistrationUrl);
    const auto registration_request = parse_json(registration.body);
    const auto* object = ai::json_object(registration_request);
    REQUIRE(object != nullptr);
    const auto* redirect_uris = ai::json_array_member(*object, "redirect_uris");
    REQUIRE(redirect_uris != nullptr);
    REQUIRE(redirect_uris->size() == 1);
    CHECK(redirect_uris->front().get_string() == kRedirectUrl);
    CHECK(ai::json_string_member(*object, "application_type") == std::optional<std::string_view>{"native"});
    CHECK(ai::json_string_member(*object, "scope") == std::optional<std::string_view>{"mcp.read mcp.write"});
    CHECK(ai::json_string_member(*object, "client_name") == std::optional<std::string_view>{"pi"});
    CHECK(ai::json_string_member(*object, "token_endpoint_auth_method") == std::optional<std::string_view>{"none"});

    // The authorization URL carries the registered client, PKCE S256, the
    // redirect URI, a fresh state, the resource, and the same scope.
    const auto& url = *outcome->authorization_url;
    CHECK(url.starts_with("https://auth.example.com/authorize?"));
    CHECK(query_param(url, "response_type") == "code");
    CHECK(query_param(url, "client_id") == "dyn-client-1");
    CHECK(query_param(url, "code_challenge_method") == "S256");
    CHECK_FALSE(query_param(url, "code_challenge").empty());
    CHECK(query_param(url, "redirect_uri") == kRedirectUrl);
    CHECK(query_param(url, "scope") == "mcp.read mcp.write");
    CHECK(query_param(url, "resource") == "https://mcp.example.com/mcp");
    CHECK_FALSE(query_param(url, "state").empty());

    // The registration and the PKCE verifier are stored under the server's key.
    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->client_information.has_value());
    CHECK(stored->client_information->client_id == "dyn-client-1");
    CHECK(stored->code_verifier.has_value());
    CHECK(stored->oauth_state == std::optional<std::string>{query_param(url, "state")});
    CHECK_FALSE(stored->tokens.has_value());
}

TEST_CASE("the authorization code exchange stores the tokens with the granted scope",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;

    // A first pass registers the client and stores the PKCE verifier.
    auto redirect = harness.run(runtime);
    REQUIRE(redirect.has_value());
    REQUIRE(redirect->result == mcp::McpOAuthFlowResult::Redirect);

    mcp::McpOAuthFlowOptions options;
    options.authorization_code = "code-1";
    options.scope = "mcp.read mcp.write";
    auto authorized = harness.run(runtime, options);
    REQUIRE(authorized.has_value());
    CHECK(authorized->result == mcp::McpOAuthFlowResult::Authorized);
    CHECK_FALSE(authorized->authorization_url.has_value());

    REQUIRE(harness.http->requests.size() == 2);
    const auto& exchange = harness.http->requests[1];
    CHECK(exchange.url == kTokenUrl);
    CHECK(exchange.body.find("grant_type=authorization_code") != std::string::npos);
    CHECK(exchange.body.find("code=code-1") != std::string::npos);
    CHECK(exchange.body.find("client_id=dyn-client-1") != std::string::npos);
    CHECK(exchange.body.find("code_verifier=") != std::string::npos);
    CHECK(exchange.body.find("resource=") != std::string::npos);

    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "access-1");
    CHECK(stored->tokens->refresh_token == std::optional<std::string>{"refresh-1"});
    CHECK(stored->tokens->scope == std::optional<std::string>{"mcp.read"});
    REQUIRE(stored->tokens_expire_at.has_value());
    CHECK(*stored->tokens_expire_at > ai::current_timestamp_ms());
    // No token request is sent twice: the exchange is the only POST.
    CHECK(harness.http->requests.size() == 2);
}

TEST_CASE("a stored refresh token is rotated and the rotated values are persisted",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "dyn-client-1"};
    mcp::McpOAuthTokens tokens;
    tokens.access_token = "old-access";
    tokens.token_type = "Bearer";
    tokens.refresh_token = "old-refresh";
    tokens.scope = "mcp.read";
    seeded.tokens = tokens;
    seeded.tokens_expire_at = ai::current_timestamp_ms() + 1000;
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());

    harness.http->responses[kTokenUrl] = {{200,
            R"({"access_token":"access-2","token_type":"Bearer","expires_in":3600,"refresh_token":"refresh-2"})"}};
    auto outcome = harness.run(runtime);
    REQUIRE(outcome.has_value());
    CHECK(outcome->result == mcp::McpOAuthFlowResult::Authorized);

    REQUIRE(harness.http->requests.size() == 1);
    CHECK(harness.http->requests.front().body.find("grant_type=refresh_token") != std::string::npos);
    CHECK(harness.http->requests.front().body.find("refresh_token=old-refresh") != std::string::npos);

    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "access-2");
    CHECK(stored->tokens->refresh_token == std::optional<std::string>{"refresh-2"});
    // A refresh response without `scope` keeps the scope of the grant.
    CHECK(stored->tokens->scope == std::optional<std::string>{"mcp.read"});
}

TEST_CASE("a dead refresh token invalidates the tokens and re-authorizes instead of looping",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "dyn-client-1"};
    mcp::McpOAuthTokens tokens;
    tokens.access_token = "old-access";
    tokens.token_type = "Bearer";
    tokens.refresh_token = "dead-refresh";
    seeded.tokens = tokens;
    seeded.tokens_expire_at = ai::current_timestamp_ms() + 1000;
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());
    harness.http->responses[kTokenUrl] = {
            {400, R"({"error":"invalid_grant","error_description":"refresh token is expired or revoked"})"}};

    auto outcome = harness.run(runtime);
    REQUIRE(outcome.has_value());
    // pi's `invalid_grant` rule: the tokens are dropped and the flow runs once
    // more, which now needs the user to authorize.
    CHECK(outcome->result == mcp::McpOAuthFlowResult::Redirect);

    // One refresh attempt only, and the dead grant is gone rather than kept.
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(harness.http->requests.front().body.find("grant_type=refresh_token") != std::string::npos);
    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    CHECK_FALSE(stored->tokens.has_value());
    CHECK_FALSE(stored->tokens_expire_at.has_value());
    CHECK(stored->code_verifier.has_value());
}

TEST_CASE("invalid_client invalidates the registration and registers again", "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    // A registration the server no longer knows, with a refresh token to use.
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "stale-client"};
    mcp::McpOAuthTokens tokens;
    tokens.access_token = "old-access";
    tokens.token_type = "Bearer";
    tokens.refresh_token = "old-refresh";
    seeded.tokens = tokens;
    seeded.tokens_expire_at = ai::current_timestamp_ms() + 1000;
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());
    harness.http->responses[kTokenUrl] = {{401, R"({"error":"invalid_client"})"}};
    harness.http->responses[kRegistrationUrl] = {{200, R"({"client_id":"dyn-client-2"})"}};

    auto outcome = harness.run(runtime);
    // pi's `invalid_client` rule: every credential is dropped, the flow runs
    // once more, registers a fresh client, and asks for authorization.
    REQUIRE(outcome.has_value());
    CHECK(outcome->result == mcp::McpOAuthFlowResult::Redirect);
    REQUIRE(harness.http->requests.size() == 2);
    CHECK(harness.http->requests[0].url == kTokenUrl);
    CHECK(harness.http->requests[1].url == kRegistrationUrl);
    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->client_information.has_value());
    CHECK(stored->client_information->client_id == "dyn-client-2");
    CHECK_FALSE(stored->tokens.has_value());
    CHECK(query_param(*outcome->authorization_url, "client_id") == "dyn-client-2");
}

TEST_CASE("an invalid_client during a code exchange also drops the PKCE verifier",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "stale-client"};
    seeded.code_verifier = "verifier-1";
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());
    harness.http->responses[kTokenUrl] = {{401, R"({"error":"invalid_client"})"}};
    harness.http->responses[kRegistrationUrl] = {{200, R"({"client_id":"dyn-client-2"})"}};

    mcp::McpOAuthFlowOptions options;
    options.authorization_code = "code-1";
    auto outcome = harness.run(runtime, options);
    // pi drops the registration with the rest of the credentials, and a code
    // exchange without client information is an explicit failure rather than a
    // silent re-registration: the sign-in must start over. The stale
    // registration is still gone, which is the point of the retry rule.
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "OAuth client information is missing during code exchange");
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(harness.http->requests.front().url == kTokenUrl);
    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    CHECK_FALSE(stored->client_information.has_value());
    CHECK_FALSE(stored->code_verifier.has_value());
}

TEST_CASE("SEP-837 application_type is native for loopback and custom-scheme redirect URIs",
        "[coding_agent][mcp][issue884][spec]") {
    CHECK(mcp::derive_application_type({"http://127.0.0.1:41234/callback"}) == "native");
    CHECK(mcp::derive_application_type({"http://localhost:41234/callback"}) == "native");
    CHECK(mcp::derive_application_type({"http://[::1]:41234/callback"}) == "native");
    CHECK(mcp::derive_application_type({"com.example.app:/oauth/callback"}) == "native");
    CHECK(mcp::derive_application_type({"https://app.example.com/callback"}) == "web");
    // One native redirect URI makes the whole client native.
    CHECK(mcp::derive_application_type({"https://app.example.com/callback", "http://127.0.0.1:1/callback"}) ==
            "native");
}

TEST_CASE("a Client ID Metadata Document needs a public-client-capable server", "[coding_agent][mcp][issue884][spec]") {
    mcp::McpAuthorizationServerMetadata supported;
    supported.issuer = "https://auth.example.com";
    supported.authorization_endpoint = "https://auth.example.com/authorize";
    supported.token_endpoint = "https://auth.example.com/token";
    supported.response_types_supported = {"code"};
    supported.client_id_metadata_document_supported = true;
    supported.token_endpoint_auth_methods_supported = {"none"};

    // Without RFC 9207 `iss`, the document and the redirect URI are specific to
    // the MCP server.
    auto specific = mcp::client_metadata_document(kServerUrl, kRedirectUrl, supported);
    REQUIRE(specific.has_value());
    CHECK(specific->url.starts_with("https://pi.dev/oauth/"));
    CHECK(specific->url.ends_with("/client.json"));
    CHECK(specific->redirect_url.starts_with("http://127.0.0.1:41234/callback/"));

    mcp::McpAuthorizationServerMetadata with_iss = supported;
    with_iss.authorization_response_iss_parameter_supported = true;
    auto shared = mcp::client_metadata_document(kServerUrl, kRedirectUrl, with_iss);
    REQUIRE(shared.has_value());
    CHECK(shared->url == "https://pi.dev/oauth/client.json");
    CHECK(shared->redirect_url == kRedirectUrl);

    // A server with no support (or no `none` auth method) is an explicit
    // failure carrying pi's message.
    mcp::McpAuthorizationServerMetadata unsupported = supported;
    unsupported.client_id_metadata_document_supported = std::nullopt;
    auto rejected = mcp::client_metadata_document(kServerUrl, kRedirectUrl, unsupported);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.find("does not support Client ID Metadata Documents") != std::string::npos);
}

TEST_CASE("cimd mode identifies through the metadata document without registering",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    harness.oauth.client_registration = mcp::McpClientRegistration::Cimd;
    harness.authorization_server_document =
            R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)" +
            std::string{R"("token_endpoint":"https://auth.example.com/token",)"} +
            std::string{R"("response_types_supported":["code"],)"} +
            std::string{R"("client_id_metadata_document_supported":true,)"} +
            std::string{R"("token_endpoint_auth_methods_supported":["none"]})"};

    auto outcome = harness.run(runtime);
    REQUIRE(outcome.has_value());
    CHECK(outcome->result == mcp::McpOAuthFlowResult::Redirect);
    // No registration POST at all: the document URL is the client id.
    CHECK(harness.http->requests.empty());
    const auto& url = *outcome->authorization_url;
    CHECK(query_param(url, "client_id").starts_with("https://pi.dev/oauth/"));
    CHECK(query_param(url, "redirect_uri").starts_with("http://127.0.0.1:41234/callback/"));
}

TEST_CASE("an authorization code from another issuer never reaches the token endpoint",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "dyn-client-1"};
    seeded.code_verifier = "verifier-1";
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());
    harness.authorization_server_document =
            R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)" +
            std::string{R"("token_endpoint":"https://auth.example.com/token",)"} +
            std::string{R"("response_types_supported":["code"],)"} +
            std::string{R"("authorization_response_iss_parameter_supported":true,)"} +
            std::string{R"("token_endpoint_auth_methods_supported":["none"]})"};

    mcp::McpOAuthFlowOptions options;
    options.authorization_code = "code-1";
    options.iss = "https://attacker.example.com";
    auto outcome = harness.run(runtime, options);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("OAuth issuer mismatch") != std::string::npos);
    CHECK(harness.http->requests.empty());

    // The matching `iss` is exchanged.
    options.iss = "https://auth.example.com";
    auto authorized = harness.run(runtime, options);
    REQUIRE(authorized.has_value());
    CHECK(authorized->result == mcp::McpOAuthFlowResult::Authorized);
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(harness.http->requests.front().url == kTokenUrl);
}

TEST_CASE("a server that does not support the authorization code grant or PKCE S256 is rejected",
        "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "dyn-client-1"};
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());

    harness.authorization_server_document =
            R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)" +
            std::string{R"("token_endpoint":"https://auth.example.com/token",)"} +
            std::string{R"("response_types_supported":["token"],)"} +
            std::string{R"("code_challenge_methods_supported":["plain"]})"};
    auto no_code = harness.run(runtime);
    REQUIRE_FALSE(no_code.has_value());
    CHECK(no_code.error().message == "Authorization server does not support authorization codes");

    harness.authorization_server_document =
            R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)" +
            std::string{R"("token_endpoint":"https://auth.example.com/token",)"} +
            std::string{R"("response_types_supported":["code"],)"} +
            std::string{R"("code_challenge_methods_supported":["plain"]})"};
    auto no_s256 = harness.run(runtime);
    REQUIRE_FALSE(no_s256.has_value());
    CHECK(no_s256.error().message == "Authorization server does not support PKCE S256");
}

TEST_CASE("a server_error refresh is quiet and falls through to authorization", "[coding_agent][mcp][issue884][spec]") {
    Harness harness;
    tests::RuntimeFixture runtime;
    mcp::McpOAuthState seeded;
    seeded.server_url = kServerUrl;
    seeded.client_information = mcp::McpOAuthClientInformation{.client_id = "dyn-client-1"};
    mcp::McpOAuthTokens tokens;
    tokens.access_token = "old-access";
    tokens.token_type = "Bearer";
    tokens.refresh_token = "old-refresh";
    seeded.tokens = tokens;
    REQUIRE(harness.store->save("echo", kServerUrl, seeded).has_value());
    harness.http->responses[kTokenUrl] = {{503, "upstream unavailable"}};

    auto outcome = harness.run(runtime);
    REQUIRE(outcome.has_value());
    CHECK(outcome->result == mcp::McpOAuthFlowResult::Redirect);
}
