// The interactive and request-time sides of the MCP OAuth flow (spec #882,
// ticket #884): `sign_in_mcp_server` drives the landed discovery/DCR/PKCE flow
// through a real loopback callback that races a pasted redirect URL, and
// `McpOAuthFlowTokenResolver` resolves an `oauth` block's tokens through the
// flow without pre-resolved endpoints. pi source at `7c10bd43`:
// `packages/coding-agent/src/extensions/mcp/oauth.ts` (`signInMcpServer`,
// `createMcpAuthProvider`, `responseFromRedirectUrl`). The authorization
// server is the scripted `OAuthHttpClient` seam and the callback is a real
// loopback listener; no live network is used.
//
// The separating cases: a pasted URL that names another redirect URI must not
// complete the sign-in, and a server with no stored credential must fail the
// request with the shared re-login error rather than reach the server
// unauthenticated.

#include <cch/ai/JsonAccess.hpp>
#include <cch/ai/OAuthHttpClient.hpp>
#include <cch/ai/Pkce.hpp>
#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthSignIn.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/Json.hpp"
#include "support/ReadyResult.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Timestamps.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace mcp = coding_agent::mcp;

namespace {

const std::string kServerUrl = "https://mcp.example.com/mcp";
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

[[nodiscard]] std::string query_param(std::string_view url, std::string_view key) {
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return {};
    }
    const auto pairs = ai::auth::parse_query_pairs(url.substr(query_start + 1));
    const auto found = pairs.find(std::string{key});
    return found == pairs.end() ? std::string{} : found->second;
}

struct CallbackEndpoint {
    std::string host;
    std::uint16_t port{0};
    std::string path;
};

[[nodiscard]] CallbackEndpoint callback_endpoint(std::string_view callback_url) {
    const auto authority_start = callback_url.find("://");
    const auto host_start = authority_start == std::string_view::npos ? 0 : authority_start + 3;
    const auto path_start = callback_url.find('/', host_start);
    const auto authority = callback_url.substr(host_start,
            path_start == std::string_view::npos ? callback_url.size() - host_start : path_start - host_start);
    const auto port_start = authority.rfind(':');
    if (port_start == std::string_view::npos) {
        return {};
    }
    CallbackEndpoint endpoint;
    endpoint.host = std::string{authority.substr(0, port_start)};
    const auto port_text = authority.substr(port_start + 1);
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), endpoint.port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size()) {
        return {};
    }
    endpoint.path = path_start == std::string_view::npos ? "/" : std::string{callback_url.substr(path_start)};
    return endpoint;
}

[[nodiscard]] boost::asio::awaitable<std::pair<int, std::string>> http_get(
        std::string host, std::uint16_t port, std::string target) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = asio::ip::tcp;

    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(tcp::endpoint(asio::ip::make_address(host), port), asio::use_awaitable);
    http::request<http::string_body> request{http::verb::get, target, 11};
    request.set(http::field::host, host);
    co_await http::async_write(socket, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(socket, buffer, response, asio::use_awaitable);
    socket.close();
    co_return std::pair{static_cast<int>(response.result_int()), std::move(response.body())};
}

/// A paste prompt that never resolves until its stop token fires.
[[nodiscard]] support::AsyncResult<std::optional<std::string>> block_until_stop(std::stop_token stop) {
    return support::detail::make_async_result(
            [stop]() -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                auto executor = co_await boost::asio::this_coro::executor;
                auto timer = std::make_shared<boost::asio::steady_timer>(executor, std::chrono::hours{1});
                std::stop_callback cancel{stop, [timer] { timer->cancel(); }};
                boost::system::error_code wait_error;
                co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
                co_return std::optional<std::string>{};
            });
}

struct SignInHarness {
    std::shared_ptr<tests::FakeOAuthHttpClient> http = std::make_shared<tests::FakeOAuthHttpClient>();
    tests::TempWorkspace workspace;
    std::shared_ptr<mcp::McpAuthStore> store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    mcp::McpOAuthConfig oauth;
    std::string resource_document{kResourceDocument};
    std::string authorization_server_document{kAuthorizationServerDocument};
    /// The pasted redirect URL suffix, or `std::nullopt` to leave the prompt
    /// blocked so the browser callback must win.
    std::optional<std::string> paste_suffix{std::string{"code=pasted-code"}};
    std::optional<std::string> authorization_url{std::nullopt};
    bool prompt_called{false};

    SignInHarness() { http->responses[kRegistrationUrl] = {{200, kRegistrationResponse}}; }

    [[nodiscard]] support::ExpectedVoid run(bool drive_callback) {
        boost::asio::io_context io;
        auto executor = io.get_executor();
        // Two authorize_mcp runs (redirect then exchange) each rediscover the
        // authorization server; script extra answers for the flow's retry
        // rules.
        http->get_responses[kResourceMetadataUrl].assign({{200, resource_document}, {200, resource_document}});
        http->get_responses[kAuthorizationServerMetadataUrl].assign(
                {{200, authorization_server_document}, {200, authorization_server_document}});
        using UrlChannel = boost::asio::experimental::channel<void(boost::system::error_code, std::string)>;
        auto url_seen = std::make_shared<UrlChannel>(executor, 1);

        auto future = boost::asio::co_spawn(
                io,
                [this, url_seen]() -> boost::asio::awaitable<support::ExpectedVoid> {
                    mcp::McpOAuthSignInRequest request;
                    request.store = store;
                    request.server_name = "echo";
                    request.server_url = kServerUrl;
                    request.oauth = oauth;
                    request.timeout = std::chrono::seconds{5};
                    request.http = http;
                    request.prompt.show_authorization_url = [this, url_seen](const std::string& url) {
                        authorization_url = url;
                        url_seen->try_send(boost::system::error_code{}, url);
                    };
                    if (paste_suffix) {
                        const std::string suffix = *paste_suffix;
                        request.prompt.prompt_for_redirect_url =
                                [this, suffix](std::stop_token) -> support::AsyncResult<std::optional<std::string>> {
                            prompt_called = true;
                            const std::string redirect =
                                    query_param(authorization_url.value_or(std::string{}), "redirect_uri");
                            const std::string state = query_param(authorization_url.value_or(std::string{}), "state");
                            return tests::ready_result(
                                    std::optional<std::string>{redirect + "?" + suffix + "&state=" + state});
                        };
                    } else {
                        request.prompt.prompt_for_redirect_url = [](std::stop_token stop) {
                            return block_until_stop(stop);
                        };
                    }
                    co_return co_await support::detail::await_async_result(mcp::sign_in_mcp_server(std::move(request)));
                },
                boost::asio::use_future);

        if (drive_callback) {
            boost::asio::co_spawn(
                    io,
                    [url_seen]() -> boost::asio::awaitable<void> {
                        boost::system::error_code receive_error;
                        const std::string url = co_await url_seen->async_receive(
                                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                        if (receive_error) {
                            co_return;
                        }
                        const auto redirect = query_param(url, "redirect_uri");
                        const auto state = query_param(url, "state");
                        const auto endpoint = callback_endpoint(redirect);
                        (void)co_await http_get(
                                endpoint.host, endpoint.port, endpoint.path + "?code=loopback-code&state=" + state);
                    },
                    boost::asio::detached);
        }
        io.run();
        return future.get();
    }

    [[nodiscard]] std::optional<mcp::McpOAuthState> state() {
        auto stored = store->load("echo", kServerUrl);
        REQUIRE(stored.has_value());
        return std::move(*stored);
    }
};

} // namespace

TEST_CASE("a pasted redirect URL completes the MCP OAuth sign-in", "[coding_agent][mcp][issue884][spec]") {
    SignInHarness harness;
    harness.http->responses[kTokenUrl] = {{200, kTokenResponse}};

    auto outcome = harness.run(/* drive_callback */ false);
    REQUIRE(outcome.has_value());

    // The flow registered a client, then exchanged the pasted code.
    REQUIRE(harness.authorization_url.has_value());
    CHECK(query_param(*harness.authorization_url, "client_id") == "dyn-client-1");
    CHECK(query_param(*harness.authorization_url, "redirect_uri").starts_with("http://127.0.0.1:"));
    CHECK(harness.prompt_called);

    REQUIRE(harness.http->requests.size() == 2);
    CHECK(harness.http->requests[0].url == "https://auth.example.com/register");
    const auto& exchange = harness.http->requests[1];
    CHECK(exchange.url == kTokenUrl);
    CHECK(exchange.body.find("grant_type=authorization_code") != std::string::npos);
    CHECK(exchange.body.find("code=pasted-code") != std::string::npos);
    CHECK(exchange.body.find("code_verifier=") != std::string::npos);

    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "access-1");
    CHECK(stored->tokens->refresh_token == std::optional<std::string>{"refresh-1"});
    REQUIRE(stored->client_information.has_value());
    CHECK(stored->client_information->client_id == "dyn-client-1");
    // The registered redirect URI is kept so a request-time refresh sends the
    // same one.
    REQUIRE_FALSE(stored->client_information->redirect_uris.empty());
}

TEST_CASE("the browser callback completes the MCP OAuth sign-in", "[coding_agent][mcp][issue884][spec]") {
    SignInHarness harness;
    harness.paste_suffix = std::nullopt;
    harness.http->responses[kTokenUrl] = {{200, kTokenResponse}};

    auto outcome = harness.run(/* drive_callback */ true);
    REQUIRE(outcome.has_value());

    REQUIRE(harness.http->requests.size() == 2);
    CHECK(harness.http->requests[1].body.find("code=loopback-code") != std::string::npos);
    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "access-1");
}

TEST_CASE("a server-specific Client ID Metadata Document signs in on its callback path",
        "[coding_agent][mcp][issue884][spec]") {
    SignInHarness harness;
    harness.paste_suffix = std::nullopt;
    harness.oauth.client_registration = mcp::McpClientRegistration::Cimd;
    // Without RFC 9207 `iss`, the document and the redirect URI are specific
    // to this MCP server.
    harness.authorization_server_document =
            R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)" +
            std::string{R"("token_endpoint":"https://auth.example.com/token",)"} +
            std::string{R"("response_types_supported":["code"],)"} +
            std::string{R"("client_id_metadata_document_supported":true,)"} +
            std::string{R"("token_endpoint_auth_methods_supported":["none"]})"};
    harness.http->responses[kTokenUrl] = {{200, kTokenResponse}};

    auto outcome = harness.run(/* drive_callback */ true);
    REQUIRE(outcome.has_value());

    // No registration POST: the document URL is the client id and the
    // callback arrived on `/callback/<callback id>`.
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(harness.http->requests.front().url == kTokenUrl);
    REQUIRE(harness.authorization_url.has_value());
    CHECK(query_param(*harness.authorization_url, "client_id").starts_with("https://pi.dev/oauth/"));
    CHECK(query_param(*harness.authorization_url, "redirect_uri").find("/callback/") != std::string::npos);
    const auto stored = harness.state();
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "access-1");
}

TEST_CASE("a configured oauth scope is requested instead of the server's default",
        "[coding_agent][mcp][issue884][spec]") {
    SignInHarness harness;
    harness.oauth.scope = "custom.scope";
    harness.http->responses[kTokenUrl] = {{200, kTokenResponse}};

    auto outcome = harness.run(/* drive_callback */ false);
    REQUIRE(outcome.has_value());
    REQUIRE(harness.authorization_url.has_value());
    CHECK(query_param(*harness.authorization_url, "scope") == "custom.scope");
    // The registration asks for the same scope.
    REQUIRE(harness.http->requests.size() == 2);
    CHECK(harness.http->requests[0].body.find("custom.scope") != std::string::npos);
}

TEST_CASE("a pasted redirect URL for another redirect URI does not complete the sign-in",
        "[coding_agent][mcp][issue884][spec]") {
    SignInHarness harness;
    // The paste names an unrelated redirect URI: the sign-in must not accept it.
    harness.paste_suffix = std::nullopt;
    harness.prompt_called = false;
    harness.http->responses[kTokenUrl] = {{200, kTokenResponse}};
    auto outcome = [&]() {
        boost::asio::io_context io;
        auto executor = io.get_executor();
        harness.http->get_responses[kResourceMetadataUrl].assign({{200, kResourceDocument}, {200, kResourceDocument}});
        harness.http->get_responses[kAuthorizationServerMetadataUrl].assign(
                {{200, kAuthorizationServerDocument}, {200, kAuthorizationServerDocument}});
        auto future = boost::asio::co_spawn(
                io,
                [&harness]() -> boost::asio::awaitable<support::ExpectedVoid> {
                    mcp::McpOAuthSignInRequest request;
                    request.store = harness.store;
                    request.server_name = "echo";
                    request.server_url = kServerUrl;
                    request.oauth = harness.oauth;
                    request.timeout = std::chrono::seconds{5};
                    request.http = harness.http;
                    request.prompt.show_authorization_url = [&harness](const std::string& url) {
                        harness.authorization_url = url;
                    };
                    request.prompt.prompt_for_redirect_url =
                            [](std::stop_token) -> support::AsyncResult<std::optional<std::string>> {
                        return tests::ready_result(
                                std::optional<std::string>{"http://127.0.0.1:1/elsewhere?code=code-1&state=x"});
                    };
                    co_return co_await support::detail::await_async_result(mcp::sign_in_mcp_server(std::move(request)));
                },
                boost::asio::use_future);
        (void)executor;
        io.run();
        return future.get();
    }();

    REQUIRE_FALSE(outcome.has_value());
    INFO(outcome.error().message);
    CHECK(outcome.error().message.find("does not match this sign-in's redirect URI") != std::string::npos);
    // The code never reaches the token endpoint.
    CHECK(harness.http->requests.size() == 1);
    CHECK(harness.http->requests.front().url == "https://auth.example.com/register");
}

// ── request-time resolution ────────────────────────────────────────────────

namespace {

[[nodiscard]] mcp::McpOAuthState seeded_state(std::string access, std::string refresh, std::int64_t expires) {
    mcp::McpOAuthState state;
    state.server_url = kServerUrl;
    mcp::McpOAuthTokens tokens;
    tokens.access_token = std::move(access);
    tokens.token_type = "Bearer";
    if (!refresh.empty()) {
        tokens.refresh_token = std::move(refresh);
    }
    state.tokens = std::move(tokens);
    state.tokens_expire_at = expires;
    mcp::McpOAuthClientInformation client;
    client.client_id = "dyn-client-1";
    client.redirect_uris = {"http://127.0.0.1:41234/callback"};
    state.client_information = std::move(client);
    return state;
}

} // namespace

TEST_CASE("a stored unexpired token is sent without any OAuth request", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    REQUIRE(store->save("echo",
                         kServerUrl,
                         seeded_state("live-access", "live-refresh", ai::current_timestamp_ms() + 3600 * 1000))
                    .has_value());
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    auto resolver =
            std::make_shared<mcp::McpOAuthFlowTokenResolver>(store, "echo", kServerUrl, mcp::McpOAuthConfig{}, http);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(headers.has_value());
    CHECK(headers->at("Authorization") == "Bearer live-access");
    CHECK(http->requests.empty());
    CHECK(http->get_requests.empty());
}

TEST_CASE("an expiring token is refreshed through the flow and persisted", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    REQUIRE(store->save("echo",
                         kServerUrl,
                         seeded_state("old-access", "old-refresh", ai::current_timestamp_ms() + 1000))
                    .has_value());
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses[kResourceMetadataUrl].assign({{200, kResourceDocument}});
    http->get_responses[kAuthorizationServerMetadataUrl].assign({{200, kAuthorizationServerDocument}});
    http->responses[kTokenUrl] = {{200,
            R"({"access_token":"new-access","token_type":"Bearer","expires_in":3600,"refresh_token":"new-refresh"})"}};
    auto resolver =
            std::make_shared<mcp::McpOAuthFlowTokenResolver>(store, "echo", kServerUrl, mcp::McpOAuthConfig{}, http);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(headers.has_value());
    CHECK(headers->at("Authorization") == "Bearer new-access");
    REQUIRE(http->requests.size() == 1);
    CHECK(http->requests.front().body.find("grant_type=refresh_token") != std::string::npos);

    auto stored = store->load("echo", kServerUrl);
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    REQUIRE((*stored)->tokens.has_value());
    CHECK((*stored)->tokens->access_token == "new-access");
}

TEST_CASE("no stored credential fails the request closed and caches the registered client",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses[kResourceMetadataUrl].assign({{200, kResourceDocument}});
    http->get_responses[kAuthorizationServerMetadataUrl].assign({{200, kAuthorizationServerDocument}});
    http->responses[kRegistrationUrl] = {{200, kRegistrationResponse}};
    auto resolver =
            std::make_shared<mcp::McpOAuthFlowTokenResolver>(store, "echo", kServerUrl, mcp::McpOAuthConfig{}, http);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE_FALSE(headers.has_value());
    CHECK(headers.error().code == support::ErrorCode::OAuth);
    CHECK(headers.error().message.find("re-authenticate") != std::string::npos);
    CHECK(headers.error().message.find("mcp__echo") != std::string::npos);
    // The client registration is cached so the later sign-in reuses it.
    auto stored = store->load("echo", kServerUrl);
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    REQUIRE((*stored)->client_information.has_value());
    CHECK((*stored)->client_information->client_id == "dyn-client-1");
    CHECK_FALSE((*stored)->tokens.has_value());
}
