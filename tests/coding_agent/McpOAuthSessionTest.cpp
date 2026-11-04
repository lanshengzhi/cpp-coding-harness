// Spec #865 fourth slice (#875), reworked for #884: MCP OAuth through the
// existing credential semantics. The provider reuses the shared `ai::auth`
// PKCE/callback helpers, the existing `AuthInteraction` presentation, and the
// existing `cch::mcp::OAuthCredential` contract; the state is stored in
// `<agentDir>/mcp-auth.json` under `mcp__<server>|<url>` and resolved per
// request. The committed loopback exchange replay is
// `fixtures/pi-mcp/oauth/replay.json`; the token transport is the scripted
// `OAuthHttpClient` seam and the callback itself is a real loopback listener,
// so no live provider or network is used.
//
// The acceptance cases pair reuse with the separating cases: a stored refresh
// token that yields `invalid_grant` produces an explicit re-login error and no
// second attempt (not a loop), and a server configured without OAuth is never
// sent a credential.

#include <cch/support/JsonAccess.hpp>
#include "support/BoostBeastStreamTransport.hpp"
#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpOAuthSignIn.hpp"
#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/Json.hpp"
#include "support/McpTestKit.hpp"
#include "support/ReadyResult.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include <cch/ai/Timestamps.hpp>
#include <cch/mcp/Auth.hpp>
#include <cch/mcp/HttpClient.hpp>
#include <cch/mcp/OAuthConfig.hpp>
#include <cch/mcp/OAuthFlow.hpp>
#include <cch/mcp/OAuthProvider.hpp>
#include <cch/mcp/OAuthServerConfig.hpp>
#include <cch/mcp/OAuthState.hpp>
#include <cch/mcp/Pkce.hpp>
#include <cch/mcp/RequestAuthSource.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;

namespace app_mcp = coding_agent::mcp;

namespace {

// ── committed replay fixture ────────────────────────────────────────────────

[[nodiscard]] support::JsonValue read_replay_fixture() {
    const auto path = std::filesystem::path(CCH_SOURCE_DIR) / "fixtures" / "pi-mcp" / "oauth" / "replay.json";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream contents;
    contents << input.rdbuf();
    auto parsed = support::read_json(contents.str());
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] support::JsonValue::object_t fixture_object(const support::JsonValue& fixture, std::string_view key) {
    const auto* object = support::json_object(fixture);
    REQUIRE(object != nullptr);
    const auto* member = support::json_object_member(*object, key);
    REQUIRE(member != nullptr);
    return *member;
}

[[nodiscard]] std::string fixture_string(const support::JsonValue::object_t& object, std::string_view key) {
    const auto value = support::json_string_member(object, key);
    REQUIRE(value.has_value());
    return std::string{*value};
}

[[nodiscard]] cch::mcp::OAuthServerConfig server_config_from_fixture(const support::JsonValue& fixture) {
    const auto& server = fixture_object(fixture, "server");
    return cch::mcp::OAuthServerConfig{
            .authorization_url = fixture_string(server, "authorization_url"),
            .token_url = fixture_string(server, "token_url"),
            .client_id = fixture_string(server, "client_id"),
            .scope = fixture_string(server, "scope"),
    };
}

// ── loopback URL helpers ────────────────────────────────────────────────────

struct CallbackEndpoint {
    std::string host{};
    std::uint16_t port{0};
    std::string path{};
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

[[nodiscard]] std::string query_param(std::string_view url, std::string_view key) {
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return {};
    }
    const auto pairs = cch::mcp::parse_query_pairs(url.substr(query_start + 1));
    const auto found = pairs.find(std::string{key});
    return found == pairs.end() ? std::string{} : found->second;
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

// ── login harness (bare loop so the callback GET races the login coroutine) ──

using SignalChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

struct LoginHarness {
    std::shared_ptr<tests::FakeOAuthHttpClient> http = std::make_shared<tests::FakeOAuthHttpClient>();
    std::shared_ptr<app_mcp::McpAuthStore> store;
    std::string server_name{"echo"};
    std::string server_url{"https://echo.example.com/mcp"};
    std::optional<std::string> auth_url{std::nullopt};
    std::vector<cch::mcp::AuthEvent> events{};
    std::vector<cch::mcp::AuthPrompt> prompts{};
    bool manual_prompt_cancelled{false};

    /// Runs the loop to completion. `on_auth_url` (if set) drives the loopback
    /// callback once the authorization URL is observed.
    [[nodiscard]] support::Expected<void> run(const cch::mcp::OAuthServerConfig& config,
            std::move_only_function<boost::asio::awaitable<void>(std::string)> on_auth_url) {
        boost::asio::io_context io;
        auto executor = io.get_executor();
        tests::EnvVarGuard callback_host_guard{"PI_OAUTH_CALLBACK_HOST"};
        callback_host_guard.unset();

        cch::mcp::AuthInteraction interaction;
        interaction.notify = [this](const cch::mcp::AuthEvent& event) {
            events.push_back(event);
            if (const auto* url = std::get_if<cch::mcp::AuthUrl>(&event.kind)) {
                auth_url = url->url;
            }
        };
        interaction.prompt = [this, executor](cch::mcp::AuthPrompt prompt) -> support::AsyncResult<std::string> {
            auto cancelled = std::make_shared<SignalChannel>(executor, 1);
            return support::detail::make_async_result(
                    [this, cancelled, prompt = std::move(prompt)]() mutable
                            -> boost::asio::awaitable<support::Expected<std::string>> {
                        prompts.push_back(prompt);
                        if (!prompt.stop_token) {
                            co_return std::unexpected(
                                    support::make_error(support::ErrorCode::OAuth, "missing manual prompt stop token"));
                        }
                        std::stop_callback callback{*prompt.stop_token, [this, cancelled] {
                                                        manual_prompt_cancelled = true;
                                                        cancelled->try_send(boost::system::error_code{});
                                                    }};
                        boost::system::error_code receive_error;
                        co_await cancelled->async_receive(
                                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                        co_return std::unexpected(
                                support::make_error(support::ErrorCode::Cancelled, "Login cancelled"));
                    });
        };

        auto url_seen =
                std::make_shared<boost::asio::experimental::channel<void(boost::system::error_code, std::string)>>(
                        executor, 1);
        auto original_notify = std::move(interaction.notify);
        interaction.notify = [url_seen, original_notify = std::move(original_notify)](
                                     const cch::mcp::AuthEvent& event) mutable {
            if (original_notify) {
                original_notify(event);
            }
            if (const auto* url = std::get_if<cch::mcp::AuthUrl>(&event.kind)) {
                url_seen->try_send(boost::system::error_code{}, url->url);
            }
        };

        auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
        auto store = this->store;
        auto server_name = this->server_name;
        auto server_url = this->server_url;
        auto login_future = boost::asio::co_spawn(
                io,
                [provider, store, server_name, server_url, interaction = std::move(interaction)]() mutable
                        -> boost::asio::awaitable<support::ExpectedVoid> {
                    co_return co_await support::detail::await_async_result(app_mcp::login_mcp_server(
                            store, server_name, server_url, *provider, std::move(interaction)));
                },
                boost::asio::use_future);

        boost::asio::co_spawn(
                io,
                [url_seen, on_auth_url = std::move(on_auth_url)]() mutable -> boost::asio::awaitable<void> {
                    std::string url;
                    boost::system::error_code receive_error;
                    url = co_await url_seen->async_receive(
                            boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                    if (receive_error) {
                        co_return;
                    }
                    if (on_auth_url) {
                        co_await on_auth_url(std::move(url));
                    }
                },
                boost::asio::detached);

        io.run();
        return login_future.get();
    }
};

// ── resolver helpers ────────────────────────────────────────────────────────

[[nodiscard]] cch::mcp::OAuthState oauth_state(std::string access, std::string refresh, std::int64_t expires) {
    cch::mcp::OAuthState state;
    state.server_url = "https://echo.example.com/mcp";
    cch::mcp::OAuthTokens tokens;
    tokens.access_token = std::move(access);
    tokens.token_type = "Bearer";
    if (!refresh.empty()) {
        tokens.refresh_token = std::move(refresh);
    }
    state.tokens = std::move(tokens);
    state.tokens_expire_at = expires;
    state.client_information = cch::mcp::OAuthClientInformation{.client_id = "pike-mcp-test-client"};
    return state;
}

void seed_state(app_mcp::McpAuthStore& store, const cch::mcp::OAuthState& state) {
    auto stored = store.save("echo", state.server_url, state);
    REQUIRE(stored.has_value());
}

/// The registered #1181 OAuth-lifecycle observation: the staged-cancellation
/// cases compare their stop point and request accounting against the frozen
/// scenario fields.
[[nodiscard]] support::JsonValue read_oauth_lifecycle_artifact() {
    const auto path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1052/oauth-lifecycle.json";
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue& oauth_lifecycle_scenario(
        const support::JsonValue& artifact, std::string_view name) {
    for (const auto& candidate : artifact.get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() == name) return candidate;
    }
    REQUIRE(false);
    return artifact;
}

[[nodiscard]] std::optional<cch::mcp::OAuthState> read_state(app_mcp::McpAuthStore& store) {
    auto stored = store.load("echo", "https://echo.example.com/mcp");
    REQUIRE(stored.has_value());
    return std::move(*stored);
}

// ── staged-cancellation discovery documents (#1102): the metadata the flow
// resolver fetches before it registers a client or exchanges a grant ─────────

const std::string kFlowResourceMetadataUrl = "https://echo.example.com/.well-known/oauth-protected-resource/mcp";
const std::string kFlowAuthorizationMetadataUrl = "https://auth.example.com/.well-known/oauth-authorization-server";
const std::string kFlowRegistrationUrl = "https://auth.example.com/register";
const std::string kFlowResourceDocument =
        R"({"resource":"https://echo.example.com/mcp","authorization_servers":["https://auth.example.com"]})";
const std::string kFlowAuthorizationServerDocument =
        R"({"issuer":"https://auth.example.com","authorization_endpoint":"https://auth.example.com/authorize",)"
        R"("token_endpoint":"https://auth.example.com/token","registration_endpoint":"https://auth.example.com/register",)"
        R"("response_types_supported":["code"],"code_challenge_methods_supported":["S256"],)"
        R"("token_endpoint_auth_methods_supported":["none"]})";

/// An expired credential whose client registration is present or absent, so
/// the flow under a stalled request stops at the discovery or the registration
/// stage.
[[nodiscard]] cch::mcp::OAuthState expired_flow_state(bool registered) {
    cch::mcp::OAuthState state;
    state.server_url = "https://echo.example.com/mcp";
    cch::mcp::OAuthTokens tokens;
    tokens.access_token = "old-access";
    tokens.token_type = "Bearer";
    tokens.refresh_token = "stalled-refresh";
    state.tokens = std::move(tokens);
    state.tokens_expire_at = ai::current_timestamp_ms() + 1000;
    if (registered) {
        state.client_information = cch::mcp::OAuthClientInformation{.client_id = "pike-mcp-test-client"};
    }
    return state;
}

/// Everything but `access` — the part a refresh must preserve or rotate.

// ── a no-credential / per-request auth source for the transport seam ─────────

class StubRequestAuth final : public cch::mcp::RequestAuthSource {
public:
    explicit StubRequestAuth(std::string token) : token_(std::move(token)) {}

    [[nodiscard]] support::AsyncResult<std::map<std::string, std::string>> current_headers(
            std::stop_token, cch::mcp::RequestAuthPurpose purpose) override {
        ++calls_;
        if (purpose == cch::mcp::RequestAuthPurpose::SessionClose) {
            ++close_calls_;
        }
        return tests::ready_result(std::map<std::string, std::string>{{"Authorization", "Bearer " + token_}});
    }

    [[nodiscard]] support::AsyncResult<bool> recover_unauthorized(std::stop_token, std::string) override {
        // A static header source has no refreshable credential: the 401 stays
        // the explicit re-login error (the separation case for recovery).
        return support::AsyncResult<bool>{false};
    }

    [[nodiscard]] int calls() const noexcept { return calls_; }
    [[nodiscard]] int close_calls() const noexcept { return close_calls_; }

private:
    std::string token_;
    int calls_{0};
    int close_calls_{0};
};

// ── real TLS MCP fixture server (mirrors the #873 harness) ───────────────────

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

[[nodiscard]] std::string tls_fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/tests/ai/providers/tls/" + std::string{name};
}

class HttpFixtureServer final {
public:
    explicit HttpFixtureServer(std::filesystem::path trace_path = {},
            std::string expected_delete_authorization = {},
            std::string revoked_authorization = {}) {
        int fds[2]{-1, -1};
        REQUIRE(::pipe(fds) == 0);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            (void)::close(fds[0]);
            (void)::dup2(fds[1], STDOUT_FILENO);
            (void)::close(fds[1]);
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)::dup2(devnull, STDERR_FILENO);
            }
            if (!trace_path.empty()) {
                (void)::setenv("PIKE_MCP_TRACE", trace_path.c_str(), 1);
            } else {
                (void)::unsetenv("PIKE_MCP_TRACE");
            }
            if (!expected_delete_authorization.empty()) {
                (void)::setenv("PIKE_MCP_EXPECT_DELETE_AUTHORIZATION", expected_delete_authorization.c_str(), 1);
            } else {
                (void)::unsetenv("PIKE_MCP_EXPECT_DELETE_AUTHORIZATION");
            }
            if (!revoked_authorization.empty()) {
                (void)::setenv("PIKE_MCP_REVOKED_AUTHORIZATION", revoked_authorization.c_str(), 1);
            } else {
                (void)::unsetenv("PIKE_MCP_REVOKED_AUTHORIZATION");
            }
            const std::string script = fixture_path("http_server.py");
            const std::string cert = tls_fixture_path("test-server.pem");
            const std::string key = tls_fixture_path("test-server-key.pem");
            ::execlp("python3",
                    "python3",
                    script.c_str(),
                    "--cert",
                    cert.c_str(),
                    "--key",
                    key.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        pid_ = child;
        (void)::close(fds[1]);
        std::string line;
        char character = '\0';
        while (::read(fds[0], &character, 1) == 1) {
            if (character == '\n') {
                break;
            }
            line.push_back(character);
        }
        (void)::close(fds[0]);
        REQUIRE(line.starts_with("PORT="));
        const std::string_view digits{line.data() + 5, line.size() - 5};
        int port = 0;
        REQUIRE(std::from_chars(digits.data(), digits.data() + digits.size(), port).ec == std::errc{});
        url_ = "https://127.0.0.1:" + std::to_string(port) + "/mcp";
    }

    HttpFixtureServer(const HttpFixtureServer&) = delete;
    HttpFixtureServer& operator=(const HttpFixtureServer&) = delete;

    ~HttpFixtureServer() {
        if (pid_ > 0) {
            (void)::kill(pid_, SIGTERM);
            int status = 0;
            (void)::waitpid(pid_, &status, 0);
        }
    }

    [[nodiscard]] const std::string& url() const noexcept { return url_; }

private:
    pid_t pid_{-1};
    std::string url_;
};

[[nodiscard]] tests::EnvVarGuard trust_test_ca() {
    return tests::EnvVarGuard{"SSL_CERT_FILE", tls_fixture_path("test-ca.pem")};
}

[[nodiscard]] app_mcp::McpHttpServerConfig http_config(const HttpFixtureServer& server) {
    app_mcp::McpHttpServerConfig config;
    config.name = "echo";
    config.url = server.url();
    return config;
}

[[nodiscard]] std::shared_ptr<cch::mcp::HttpClient> connect_http_client(tests::RuntimeFixture& runtime,
        app_mcp::McpHttpServerConfig config,
        std::shared_ptr<cch::mcp::RequestAuthSource> request_auth = nullptr) {
    auto client = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(std::move(config),
                    std::make_shared<support::BoostBeastStreamTransport>(),
                    std::move(request_auth),
                    cch::mcp::HttpGetStreamOptions{.enabled = false}));
    REQUIRE(client.has_value());
    return *client;
}

[[nodiscard]] std::string authorization_header(
        tests::RuntimeFixture& runtime, const std::shared_ptr<cch::mcp::HttpClient>& client) {
    auto response = tests::run_awaitable(
            runtime, support::detail::await_async_result(client->request("debug/authorization_present")));
    REQUIRE(response.has_value());
    const auto* object = response->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto value = object->find("authorization");
    REQUIRE(value != object->end());
    const auto* text = value->second.get_if<std::string>();
    REQUIRE(text != nullptr);
    return *text;
}

} // namespace

TEST_CASE("MCP OAuth login reuses the login surface and persists into mcp-auth.json",
        "[coding_agent][mcp][issue875][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const auto& authorize_request = fixture_object(fixture, "authorize_request");
    const auto& token_exchange = fixture_object(fixture, "token_exchange");
    const auto& token_response = fixture_object(token_exchange, "response");
    const std::string token_body = fixture_string(token_response, "body");

    tests::TempWorkspace workspace;
    LoginHarness harness;
    harness.http->responses[config.token_url] = {{200, token_body}};
    harness.store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");

    auto login = harness.run(config, [&harness](std::string url) -> boost::asio::awaitable<void> {
        const auto redirect_url = query_param(url, "redirect_uri");
        const auto callback = callback_endpoint(redirect_url);
        const auto state = query_param(url, "state");
        const auto response =
                co_await http_get(callback.host, callback.port, callback.path + "?code=loopback-code&state=" + state);
        CHECK(response.first == 200);
    });

    REQUIRE(login.has_value());

    // The authorize request is the recorded replay shape.
    REQUIRE(harness.auth_url.has_value());
    CHECK(query_param(*harness.auth_url, "response_type") == fixture_string(authorize_request, "response_type"));
    CHECK(query_param(*harness.auth_url, "client_id") == fixture_string(authorize_request, "client_id"));
    CHECK(query_param(*harness.auth_url, "code_challenge_method") ==
            fixture_string(authorize_request, "code_challenge_method"));
    CHECK(query_param(*harness.auth_url, "scope") == fixture_string(authorize_request, "scope"));
    CHECK_FALSE(query_param(*harness.auth_url, "code_challenge").empty());
    CHECK_FALSE(query_param(*harness.auth_url, "state").empty());
    CHECK(harness.manual_prompt_cancelled);

    // The authorization-code exchange is the recorded replay shape and maps to
    // the existing OAuthCredential contract.
    REQUIRE(harness.http->requests.size() == 1);
    const auto& request = harness.http->requests.front();
    CHECK(request.url == config.token_url);
    const auto content_type = request.headers.find("Content-Type");
    REQUIRE(content_type != request.headers.end());
    CHECK(content_type->second == "application/x-www-form-urlencoded");
    CHECK(request.body.find("grant_type=authorization_code") != std::string::npos);
    CHECK(request.body.find("client_id=pike-mcp-test-client") != std::string::npos);
    CHECK(request.body.find("code=loopback-code") != std::string::npos);
    CHECK(request.body.find("code_verifier=") != std::string::npos);
}

TEST_CASE("an MCP OAuth credential is stored in mcp-auth.json under mcp__<server>|<url>",
        "[coding_agent][mcp][issue875][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const auto& token_exchange = fixture_object(fixture, "token_exchange");
    const std::string token_body = fixture_string(fixture_object(token_exchange, "response"), "body");

    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");

    LoginHarness harness;
    harness.http->responses[config.token_url] = {{200, token_body}};
    harness.store = store;

    auto login = harness.run(config, [](std::string url) -> boost::asio::awaitable<void> {
        const auto callback = callback_endpoint(query_param(url, "redirect_uri"));
        const auto state = query_param(url, "state");
        (void)co_await http_get(callback.host, callback.port, callback.path + "?code=loopback-code&state=" + state);
    });
    REQUIRE(login.has_value());

    // One `mcp__<server>|<url>` key in mcp-auth.json, carrying the tokens and
    // the client information — no second credential store.
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-access-token");
    CHECK(stored->tokens->refresh_token == std::optional<std::string>{"dummy-refresh-token"});
    REQUIRE(stored->client_information.has_value());
    CHECK(stored->client_information->client_id == "pike-mcp-test-client");
    REQUIRE(stored->tokens_expire_at.has_value());
    CHECK(*stored->tokens_expire_at > ai::current_timestamp_ms());

    // Revocation removes the stored state.
    auto removed = runtime.run(app_mcp::logout_mcp_server(store, "echo", "https://echo.example.com/mcp"));
    REQUIRE(removed.has_value());
    CHECK_FALSE(read_state(*store).has_value());
}

TEST_CASE("a valid stored MCP OAuth credential resolves the bearer token without a token request",
        "[coding_agent][mcp][issue875][spec]") {
    const auto config = server_config_from_fixture(read_replay_fixture());
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("dummy-access-token",
                    "dummy-refresh-token",
                    ai::current_timestamp_ms() +
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}).count()));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(headers.has_value());
    REQUIRE(headers->contains("Authorization"));
    CHECK(headers->at("Authorization") == "Bearer dummy-access-token");
    CHECK(http->requests.empty());
}

TEST_CASE("a cancelled MCP OAuth refresh stops before the request reaches the MCP server",
        "[coding_agent][mcp][issue1102][issue1189][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{trace};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("old-access",
                    "stalled-refresh",
                    ai::current_timestamp_ms() +
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}).count()));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);
    auto client = connect_http_client(runtime, http_config(server), resolver);
    seed_state(*store, oauth_state("old-access", "stalled-refresh", ai::current_timestamp_ms() + 1000));
    // The frozen capture's `stall.add("/token")`: the refresh is in flight
    // when the request is cancelled, and `releaseAll()` lets it answer.
    auto hold_refresh = std::make_shared<tests::ReleaseGate>();
    http->hold_post = hold_refresh;
    auto oauth_request_seen = std::make_shared<bool>(false);
    http->on_post = [oauth_request_seen](const auto&) { *oauth_request_seen = true; };

    std::stop_source cancel;
    auto result = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto request_settled = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
        boost::asio::co_spawn(
                executor,
                [client, request_settled, token = cancel.get_token()]() -> boost::asio::awaitable<void> {
                    request_settled->emplace(co_await support::detail::await_async_result(client->request("tools/call",
                            tests::tools_call_params("echo", support::JsonValue::object_t{{"text", "never"}}),
                            {.stop_token = token})));
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return *oauth_request_seen; }, std::chrono::seconds{2});
        cancel.request_stop();
        co_await tests::wait_for(executor, [&] { return request_settled->has_value(); }, std::chrono::seconds{2});
        if (!request_settled->has_value()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Timeout, "refresh did not cancel"));
        }
        // The caller settled while the refresh kept running; now let the
        // token endpoint answer and wait for the shared refresh to persist.
        hold_refresh->release();
        auto refresh_settled = co_await support::detail::await_async_result(resolver->settled());
        if (!refresh_settled) {
            co_return std::unexpected(std::move(refresh_settled.error()));
        }
        co_return std::move(request_settled->value());
    }());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == support::ErrorCode::Cancelled);
    CHECK_FALSE(tests::trace_contains(trace, "recv tools/call"));
    CHECK_FALSE(tests::trace_contains(trace, "notifications/cancelled"));
    REQUIRE(http->requests.size() == 1);
    // The refresh is the shared background operation, not the cancelled
    // request's: its POST is not aborted, and it answers once released.
    CHECK_FALSE(http->requests.front().stop_token.stop_requested());
    CHECK(http->post_responses == 1);
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-rotated-access-token");
}

TEST_CASE("a timed-out MCP OAuth request settles at the deadline while the shared refresh completes",
        "[coding_agent][mcp][issue1189][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{trace};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("old-access",
                    "stalled-refresh",
                    ai::current_timestamp_ms() +
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}).count()));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);
    auto client_config = http_config(server);
    client_config.request_timeout = std::chrono::milliseconds{250};
    auto client = connect_http_client(runtime, std::move(client_config), resolver);
    seed_state(*store, oauth_state("old-access", "stalled-refresh", ai::current_timestamp_ms() + 1000));
    auto hold_refresh = std::make_shared<tests::ReleaseGate>();
    http->hold_post = hold_refresh;
    auto refresh_seen = std::make_shared<bool>(false);
    http->on_post = [refresh_seen](const auto&) { *refresh_seen = true; };

    auto result = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto request_settled = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
        boost::asio::co_spawn(
                executor,
                [client, request_settled]() -> boost::asio::awaitable<void> {
                    request_settled->emplace(co_await support::detail::await_async_result(client->request(
                            "tools/call", tests::tools_call_params("echo", support::JsonValue::object_t{}))));
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return *refresh_seen; }, std::chrono::seconds{2});
        co_await tests::wait_for(executor, [&] { return request_settled->has_value(); }, std::chrono::seconds{2});
        if (!request_settled->has_value()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Timeout, "request did not time out"));
        }
        hold_refresh->release();
        auto refresh_settled = co_await support::detail::await_async_result(resolver->settled());
        if (!refresh_settled) {
            co_return std::unexpected(std::move(refresh_settled.error()));
        }
        co_return std::move(request_settled->value());
    }());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == support::ErrorCode::Timeout);
    CHECK_FALSE(tests::trace_contains(trace, "recv tools/call"));
    CHECK_FALSE(tests::trace_contains(trace, "notifications/cancelled"));
    REQUIRE(http->requests.size() == 1);
    CHECK_FALSE(http->requests.front().stop_token.stop_requested());
    CHECK(http->post_responses == 1);
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-rotated-access-token");
}

TEST_CASE("concurrent MCP OAuth requests share one refresh", "[coding_agent][mcp][issue1189][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store, oauth_state("old-access", "stalled-refresh", ai::current_timestamp_ms() + 1000));
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);
    auto hold_refresh = std::make_shared<tests::ReleaseGate>();
    http->hold_post = hold_refresh;
    auto refresh_seen = std::make_shared<bool>(false);
    http->on_post = [refresh_seen](const auto&) { *refresh_seen = true; };

    auto outcomes = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<bool>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto first = std::make_shared<std::optional<support::Expected<std::map<std::string, std::string>>>>();
        auto second = std::make_shared<std::optional<support::Expected<std::map<std::string, std::string>>>>();
        boost::asio::co_spawn(
                executor,
                [resolver, first]() -> boost::asio::awaitable<void> {
                    first->emplace(co_await support::detail::await_async_result(resolver->current_headers()));
                },
                boost::asio::detached);
        boost::asio::co_spawn(
                executor,
                [resolver, second]() -> boost::asio::awaitable<void> {
                    second->emplace(co_await support::detail::await_async_result(resolver->current_headers()));
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return *refresh_seen; }, std::chrono::seconds{2});
        // Both requests join the one shared refresh before it answers.
        co_await tests::wait_for(executor, [&] { return false; }, std::chrono::milliseconds{50});
        hold_refresh->release();
        co_await tests::wait_for(
                executor, [&] { return first->has_value() && second->has_value(); }, std::chrono::seconds{2});
        if (!first->has_value() || !second->has_value()) {
            co_return support::Expected<bool>{false};
        }
        co_return support::Expected<bool>{first->value().has_value() && second->value().has_value() &&
                                          first->value()->at("Authorization") == "Bearer dummy-rotated-access-token" &&
                                          second->value()->at("Authorization") == "Bearer dummy-rotated-access-token"};
    }());
    REQUIRE(outcomes.has_value());
    CHECK(*outcomes);
    REQUIRE(http->requests.size() == 1);
    CHECK(http->post_responses == 1);
}

TEST_CASE("MCP OAuth refresh uses pi's 30-second skew", "[coding_agent][mcp][issue1189][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    // Two minutes of remaining lifetime is outside pi's 30-second skew (and
    // inside the five-minute margin this resolver used before #1189).
    seed_state(*store, oauth_state("long-lived", "refresh-token", ai::current_timestamp_ms() + 120'000));
    auto untouched = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(untouched.has_value());
    CHECK(untouched->at("Authorization") == "Bearer long-lived");
    CHECK(http->requests.empty());

    // Twenty seconds is inside the skew: the same resolver refreshes.
    seed_state(*store, oauth_state("near-expiry", "refresh-token", ai::current_timestamp_ms() + 20'000));
    auto refreshed = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->at("Authorization") == "Bearer dummy-rotated-access-token");
    CHECK(http->requests.size() == 1);
}

TEST_CASE("a token rotated by another process under the MCP OAuth refresh lock is reused without a second refresh",
        "[coding_agent][mcp][issue1189][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store, oauth_state("old-access", "stalled-refresh", ai::current_timestamp_ms() + 1000));
    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    // What the other process's refresh saves before it releases the lock: a
    // rotated access token whose lifetime is well outside the refresh skew.
    const std::string rotated_state =
            R"({"mcp__echo|https://echo.example.com/mcp":{"serverUrl":"https://echo.example.com/mcp",)"
            R"("clientInformation":{"client_id":"pike-mcp-test-client"},"tokens":{"access_token":"rotated-by-other-process",)"
            R"("token_type":"Bearer","refresh_token":"stalled-refresh"},"tokensExpireAt":)" +
            std::to_string(ai::current_timestamp_ms() + 3'600'000) + "}}\n";
    const auto store_path = workspace.path() / "mcp-auth.json";
    const auto lock_path = store->refresh_lock_path("echo", "https://echo.example.com/mcp");
    int ready[2]{-1, -1};
    int release[2]{-1, -1};
    REQUIRE(::pipe(ready) == 0);
    REQUIRE(::pipe(release) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        (void)::close(ready[0]);
        (void)::close(release[1]);
        const int descriptor = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor < 0 || ::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            ::_exit(2);
        }
        const char locked = '1';
        (void)::write(ready[1], &locked, 1);
        char unlock = '\0';
        (void)::read(release[0], &unlock, 1);
        // Save the rotated grant as the other process's refresh would, then
        // release the lock for the waiting process to re-read.
        const int store_descriptor = ::open(store_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (store_descriptor < 0 || ::write(store_descriptor, rotated_state.data(), rotated_state.size()) !=
                                            static_cast<ssize_t>(rotated_state.size())) {
            ::_exit(3);
        }
        (void)::close(store_descriptor);
        (void)::flock(descriptor, LOCK_UN);
        (void)::close(descriptor);
        ::_exit(0);
    }
    (void)::close(ready[1]);
    (void)::close(release[0]);
    char locked = '\0';
    REQUIRE(::read(ready[0], &locked, 1) == 1);

    auto outcome = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<bool>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto headers = std::make_shared<std::optional<support::Expected<std::map<std::string, std::string>>>>();
        boost::asio::co_spawn(
                executor,
                [resolver, headers]() -> boost::asio::awaitable<void> {
                    headers->emplace(co_await support::detail::await_async_result(resolver->current_headers()));
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return false; }, std::chrono::milliseconds{200});
        // While the other process holds the lock, this process started no
        // refresh.
        const bool waited = http->requests.empty();
        const char unlock = '1';
        (void)::write(release[1], &unlock, 1);
        co_await tests::wait_for(executor, [&] { return headers->has_value(); }, std::chrono::seconds{2});
        co_return support::Expected<bool>{waited && headers->has_value() && headers->value().has_value() &&
                                          headers->value()->at("Authorization") == "Bearer rotated-by-other-process"};
    }());
    int status = 0;
    (void)::waitpid(child, &status, 0);
    (void)::close(ready[0]);
    (void)::close(release[1]);
    REQUIRE(outcome.has_value());
    CHECK(*outcome);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    // The waiting process reused the rotated token instead of refreshing with
    // the grant the other process already consumed.
    CHECK(http->requests.empty());
    CHECK(http->post_responses == 0);
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "rotated-by-other-process");
}

TEST_CASE("a cancelled or timed-out MCP OAuth tool call never retries after the shared refresh completes",
        "[coding_agent][mcp][issue1189][spec]") {
    const bool timeout_mode = GENERATE(false, true);
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    // The fixture revokes exactly the seeded token for tools/call; the
    // initialized connection used it successfully before it was revoked.
    HttpFixtureServer server{trace, "Bearer old-access", "Bearer old-access"};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store, oauth_state("old-access", "stalled-refresh", ai::current_timestamp_ms() + 120'000));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);
    auto client_config = http_config(server);
    if (timeout_mode) {
        client_config.request_timeout = std::chrono::milliseconds{250};
    }
    auto client = connect_http_client(runtime, std::move(client_config), resolver);
    auto hold_refresh = std::make_shared<tests::ReleaseGate>();
    http->hold_post = hold_refresh;
    auto refresh_seen = std::make_shared<bool>(false);
    http->on_post = [refresh_seen](const auto&) { *refresh_seen = true; };

    std::stop_source cancel;
    auto result = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto request_settled = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
        boost::asio::co_spawn(
                executor,
                [client, request_settled, timeout_mode, token = cancel.get_token()]() -> boost::asio::awaitable<void> {
                    request_settled->emplace(co_await support::detail::await_async_result(client->request("tools/call",
                            tests::tools_call_params("echo", support::JsonValue::object_t{{"text", "never"}}),
                            {.stop_token = timeout_mode ? std::stop_token{} : token})));
                },
                boost::asio::detached);
        // The first tools/call reached the server and was rejected with 401;
        // its recovery started the shared refresh.
        co_await tests::wait_for(
                executor, [&] { return tests::trace_contains(trace, "recv 401 tools/call"); }, std::chrono::seconds{2});
        co_await tests::wait_for(executor, [&] { return *refresh_seen; }, std::chrono::seconds{2});
        if (!timeout_mode) {
            cancel.request_stop();
        }
        co_await tests::wait_for(executor, [&] { return request_settled->has_value(); }, std::chrono::seconds{2});
        if (!request_settled->has_value()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Timeout, "request did not settle"));
        }
        hold_refresh->release();
        auto refresh_settled = co_await support::detail::await_async_result(resolver->settled());
        if (!refresh_settled) {
            co_return std::unexpected(std::move(refresh_settled.error()));
        }
        co_return std::move(request_settled->value());
    }());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == (timeout_mode ? support::ErrorCode::Timeout : support::ErrorCode::Cancelled));
    // Exactly the one 401 attempt; the aborted/timed-out request is never
    // re-dispatched with the rotated token.
    CHECK(tests::trace_line_count(trace, "recv tools/call") == 1);
    REQUIRE(http->requests.size() == 1);
    CHECK_FALSE(http->requests.front().stop_token.stop_requested());
    CHECK(http->post_responses == 1);
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-rotated-access-token");
}

TEST_CASE("a cancelled MCP OAuth discovery stops before any registration or token request",
        "[coding_agent][mcp][issue1102][issue1181][spec]") {
    // Frozen pi v1.3.0 (audit-1052 oauth-lifecycle.json, "cancellation during
    // protected-resource discovery aborts the flow"): the stalled path, the
    // destroyed request socket and the zero registration/token request counts
    // compared below are the registered observation's own fields.
    const auto artifact = read_oauth_lifecycle_artifact();
    const auto& frozen_expected =
            oauth_lifecycle_scenario(artifact, "cancellation during protected-resource discovery aborts the flow")
                    .get_object()
                    .at("expected")
                    .get_object();
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{trace};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("old-access",
                    "stalled-refresh",
                    ai::current_timestamp_ms() +
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}).count()));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    auto resolver = std::make_shared<app_mcp::McpOAuthFlowTokenResolver>(
            store, "echo", "https://echo.example.com/mcp", cch::mcp::OAuthConfig{}, http);
    auto client = connect_http_client(runtime, http_config(server), resolver);
    // The call under test starts from an expired credential, so auth
    // resolution runs the whole flow; the cancellation lands on the first
    // discovery request and nothing may follow it.
    seed_state(*store, expired_flow_state(true));
    http->hold_get_until_stopped = true;
    auto discovery_seen = std::make_shared<bool>(false);
    http->on_get = [discovery_seen](const auto&) { *discovery_seen = true; };

    std::stop_source cancel;
    auto result = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto settled = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
        boost::asio::co_spawn(
                executor,
                [client, settled, token = cancel.get_token()]() -> boost::asio::awaitable<void> {
                    settled->emplace(co_await support::detail::await_async_result(client->request("tools/call",
                            tests::tools_call_params("echo", support::JsonValue::object_t{{"text", "never"}}),
                            {.stop_token = token})));
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return *discovery_seen; }, std::chrono::seconds{2});
        cancel.request_stop();
        co_await tests::wait_for(executor, [&] { return settled->has_value(); }, std::chrono::seconds{2});
        if (!settled->has_value()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Timeout, "discovery did not cancel"));
        }
        co_return std::move(**settled);
    }());
    REQUIRE_FALSE(result.has_value());
    // The frozen lifecycle contract records pi's AbortError for a cancelled
    // operation; this runtime reports it as ErrorCode::Cancelled.
    CHECK(result.error().code == support::ErrorCode::Cancelled);
    // Cancellation during protected-resource discovery must be the last
    // request: the stalled path matches the frozen document, the held request's
    // stop token is the frozen destroyed request socket, and no client
    // registration or token request follows it.
    REQUIRE(http->get_requests.size() == 1);
    CHECK(http->get_requests.front().url == kFlowResourceMetadataUrl);
    CHECK(kFlowResourceMetadataUrl.ends_with(frozen_expected.at("stalledPath").get_string()));
    CHECK(http->get_requests.front().stop_token.stop_requested() ==
            frozen_expected.at("requestSocketDestroyed").get_boolean());
    CHECK(http->requests.empty());
    CHECK(static_cast<std::size_t>(http->requests.size()) ==
            static_cast<std::size_t>(frozen_expected.at("tokenRequests").get_number()));
    CHECK_FALSE(tests::trace_contains(trace, "recv tools/call"));
    CHECK_FALSE(tests::trace_contains(trace, "notifications/cancelled"));
}

TEST_CASE("a cancelled MCP OAuth client registration stops before any token request",
        "[coding_agent][mcp][issue1102][issue1181][spec]") {
    // Frozen pi v1.3.0 (audit-1052 oauth-lifecycle.json, "cancellation during
    // dynamic client registration aborts the flow"): the stalled path and the
    // zero token/authorization request counts compared below are the
    // registered observation's own fields.
    const auto artifact = read_oauth_lifecycle_artifact();
    const auto& frozen_expected =
            oauth_lifecycle_scenario(artifact, "cancellation during dynamic client registration aborts the flow")
                    .get_object()
                    .at("expected")
                    .get_object();
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{trace};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("old-access",
                    "stalled-refresh",
                    ai::current_timestamp_ms() +
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}).count()));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->get_responses[kFlowResourceMetadataUrl] = {{200, kFlowResourceDocument}};
    http->get_responses[kFlowAuthorizationMetadataUrl] = {{200, kFlowAuthorizationServerDocument}};
    auto resolver = std::make_shared<app_mcp::McpOAuthFlowTokenResolver>(
            store, "echo", "https://echo.example.com/mcp", cch::mcp::OAuthConfig{}, http);
    auto client = connect_http_client(runtime, http_config(server), resolver);
    // No stored client registration: resolving the call's auth discovers the
    // authorization server first and then stalls on the registration request.
    seed_state(*store, expired_flow_state(false));
    http->hold_post_until_stopped = true;
    auto registration_seen = std::make_shared<bool>(false);
    http->on_post = [registration_seen](const auto&) { *registration_seen = true; };

    std::stop_source cancel;
    auto result = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto settled = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
        boost::asio::co_spawn(
                executor,
                [client, settled, token = cancel.get_token()]() -> boost::asio::awaitable<void> {
                    settled->emplace(co_await support::detail::await_async_result(client->request("tools/call",
                            tests::tools_call_params("echo", support::JsonValue::object_t{{"text", "never"}}),
                            {.stop_token = token})));
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return *registration_seen; }, std::chrono::seconds{2});
        cancel.request_stop();
        co_await tests::wait_for(executor, [&] { return settled->has_value(); }, std::chrono::seconds{2});
        if (!settled->has_value()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Timeout, "registration did not cancel"));
        }
        co_return std::move(**settled);
    }());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == support::ErrorCode::Cancelled);
    // Discovery ran to completion (the two metadata GETs); the frozen contract
    // for registration cancellation then requires the held registration
    // request to be the last one: the stalled path matches the frozen
    // document, and no token request or authorization request follows it.
    CHECK(http->get_requests.size() == 2);
    REQUIRE(http->requests.size() == 1);
    CHECK(http->requests.front().url == kFlowRegistrationUrl);
    CHECK(kFlowRegistrationUrl.ends_with(frozen_expected.at("stalledPath").get_string()));
    CHECK(http->requests.front().stop_token.stop_requested());
    // A token grant is form-encoded ("grant_type=..."); the registration
    // document only mentions "grant_types" metadata, which must not match —
    // the frozen token-request count stays zero.
    CHECK(http->requests.front().body.find("grant_type=") == std::string::npos);
    CHECK(static_cast<std::size_t>(frozen_expected.at("tokenRequests").get_number()) == 0);
    // The frozen registration-cancellation observation holds exactly one
    // request (the stalled registration); no authorization request follows.
    CHECK(static_cast<std::size_t>(frozen_expected.at("authorizations").get_number()) == 0);
    // Cancellation commits no half-registered state either: the store still
    // holds exactly the pre-flow credential.
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    CHECK_FALSE(stored->client_information.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "old-access");
    CHECK_FALSE(tests::trace_contains(trace, "recv tools/call"));
    CHECK_FALSE(tests::trace_contains(trace, "notifications/cancelled"));
}

TEST_CASE("closing an MCP HTTP client during a stalled OAuth refresh deletes the session with the stored token",
        "[coding_agent][mcp][issue1102][issue1189][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const std::string refresh_body =
            fixture_string(fixture_object(fixture_object(fixture, "refresh_exchange"), "response"), "body");
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{trace, "Bearer existing-access"};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("existing-access",
                    "stalled-refresh",
                    ai::current_timestamp_ms() +
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}).count()));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);
    auto client = connect_http_client(runtime, http_config(server), resolver);
    seed_state(*store, oauth_state("existing-access", "stalled-refresh", ai::current_timestamp_ms() + 1000));
    auto hold_refresh = std::make_shared<tests::ReleaseGate>();
    http->hold_post = hold_refresh;
    auto oauth_request_seen = std::make_shared<bool>(false);
    http->on_post = [oauth_request_seen](const auto&) { *oauth_request_seen = true; };

    auto outcome = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<bool>> {
        const auto executor = co_await boost::asio::this_coro::executor;
        auto request_settled = std::make_shared<bool>(false);
        boost::asio::co_spawn(
                executor,
                [client, request_settled]() -> boost::asio::awaitable<void> {
                    (void)co_await support::detail::await_async_result(client->request(
                            "tools/call", tests::tools_call_params("echo", support::JsonValue::object_t{})));
                    *request_settled = true;
                },
                boost::asio::detached);
        co_await tests::wait_for(executor, [&] { return *oauth_request_seen; }, std::chrono::seconds{2});
        const auto close_started = std::chrono::steady_clock::now();
        client->close();
        const auto close_elapsed = std::chrono::steady_clock::now() - close_started;
        // pi's client close aborts the transport and reuses the last token for
        // the DELETE; it never waits for the stalled refresh (#1189).
        bool close_was_prompt = http->post_responses == 0;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
        (void)close_elapsed;
#else
        close_was_prompt = close_was_prompt && close_elapsed < std::chrono::milliseconds{250};
#endif
        co_await tests::wait_for(
                executor,
                [&] { return *request_settled && tests::trace_contains(trace, "recv DELETE"); },
                std::chrono::seconds{2});
        // The refresh is still stalled after close returned; let it answer and
        // wait for the rotated grant to be persisted.
        hold_refresh->release();
        auto refresh_settled = co_await support::detail::await_async_result(resolver->settled());
        co_return support::Expected<bool>{
                close_was_prompt && refresh_settled.has_value() && *request_settled &&
                tests::trace_contains(trace, "recv DELETE authorization_present=true authorization_matches=true") &&
                http->requests.size() == 1 && !http->requests.front().stop_token.stop_requested()};
    }());
    CHECK(outcome);
    CHECK(tests::trace_contains(trace, "recv DELETE authorization_present=true authorization_matches=true"));
    CHECK_FALSE(tests::trace_contains(trace, "recv tools/call"));
    REQUIRE(http->requests.size() == 1);
    CHECK(http->post_responses == 1);
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-rotated-access-token");
}

TEST_CASE("a rotated MCP OAuth refresh is persisted before the request continues",
        "[coding_agent][mcp][issue875][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const auto& refresh_exchange = fixture_object(fixture, "refresh_exchange");
    const std::string refresh_body = fixture_string(fixture_object(refresh_exchange, "response"), "body");

    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    // Expiring within the five-minute margin, so the request-time path refreshes.
    seed_state(*store, oauth_state("dummy-old-access-token", "dummy-refresh-token", ai::current_timestamp_ms() + 1000));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(headers.has_value());
    CHECK(headers->at("Authorization") == "Bearer dummy-rotated-access-token");

    REQUIRE(http->requests.size() == 1);
    CHECK(http->requests.front().body.find("grant_type=refresh_token") != std::string::npos);
    CHECK(http->requests.front().body.find("refresh_token=dummy-refresh-token") != std::string::npos);

    // The rotated credential is persisted by the same transaction.
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-rotated-access-token");
    CHECK(stored->tokens->refresh_token == std::optional<std::string>{"dummy-rotated-refresh-token"});
}

TEST_CASE("an invalid_grant refresh is an explicit re-login error with no retry loop",
        "[coding_agent][mcp][issue875][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const auto& invalid_grant = fixture_object(fixture, "invalid_grant_exchange");
    const auto& response = fixture_object(invalid_grant, "response");

    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("dummy-old-access-token", "dummy-dead-refresh-token", ai::current_timestamp_ms() + 1000));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {
            {static_cast<int>(*support::json_integer_member(response, "status")), fixture_string(response, "body")}};
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE_FALSE(headers.has_value());
    CHECK(headers.error().code == support::ErrorCode::OAuth);
    CHECK(headers.error().message.find("re-authenticate") != std::string::npos);
    CHECK(headers.error().message.find("mcp__echo") != std::string::npos);
    // The failed exchange detail never carries a token value.
    CHECK(headers.error().message.find("dummy-") == std::string::npos);
    CHECK(headers.error().detail.find("dummy-") == std::string::npos);

    // One attempt only: a dead refresh token does not loop.
    CHECK(http->requests.size() == 1);

    // The stored state is preserved for a retry after sign-in.
    const auto stored = read_state(*store);
    REQUIRE(stored.has_value());
    REQUIRE(stored->tokens.has_value());
    CHECK(stored->tokens->access_token == "dummy-old-access-token");
    CHECK(stored->tokens->refresh_token == std::optional<std::string>{"dummy-dead-refresh-token"});
}

TEST_CASE("a missing MCP OAuth credential is an explicit re-login error, never an unauthenticated request",
        "[coding_agent][mcp][issue875][spec]") {
    const auto config = server_config_from_fixture(read_replay_fixture());
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<app_mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    auto provider = std::make_shared<cch::mcp::OAuthProvider>(config, http);
    auto resolver =
            std::make_shared<app_mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE_FALSE(headers.has_value());
    CHECK(headers.error().code == support::ErrorCode::OAuth);
    CHECK(headers.error().message.find("re-authenticate") != std::string::npos);
    CHECK(http->requests.empty());
}

TEST_CASE("the MCP HTTP transport attaches a credential only when the server needs one",
        "[coding_agent][mcp][issue875][issue1181][spec]") {
    // The frozen transports observation (audit-1052 transports.json) records
    // both authorization shapes: the same-origin redirect forwards the caller
    // credential (`authorizationForwarded`), and the cross-origin hop strips
    // it (`targetAuthorization` null). The attach-on-demand behavior below is
    // compared against those frozen fields.
    const auto transports_artifact = [] {
        const auto path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1052/transports.json";
        std::ifstream input(path, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        auto parsed = support::read_json(text);
        REQUIRE(parsed.has_value());
        return std::move(*parsed);
    }();
    const auto& frozen_forwarded = oauth_lifecycle_scenario(
            transports_artifact, "same-origin redirect is followed and keeps the request auth header")
                                           .get_object()
                                           .at("expected")
                                           .get_object()
                                           .at("authorizationForwarded")
                                           .get_boolean();
    const auto& frozen_stripped = oauth_lifecycle_scenario(
            transports_artifact, "cross-origin redirect is followed without Authorization and is rejected")
                                          .get_object()
                                          .at("expected")
                                          .get_object()
                                          .at("targetAuthorization");
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    // No request-auth source: no credentials are attached, matching the
    // frozen stripped-credential field, and no implicit sign-in is attempted.
    auto anonymous = connect_http_client(runtime, http_config(server));
    CHECK(authorization_header(runtime, anonymous).empty() == frozen_stripped.holds<std::nullptr_t>());

    // With a request-auth source the token is attached, matching the frozen
    // forwarded-credential field, and it is re-resolved for every request
    // (never cached on the connection).
    auto stub = std::make_shared<StubRequestAuth>("dummy-access-token");
    auto authenticated = connect_http_client(runtime, http_config(server), stub);
    const int before = stub->calls();
    CHECK(frozen_forwarded);
    CHECK(authorization_header(runtime, authenticated) == "Bearer dummy-access-token");
    CHECK(authorization_header(runtime, authenticated) == "Bearer dummy-access-token");
    CHECK(stub->calls() == before + 2);
    const int before_close = stub->close_calls();
    authenticated->close();
    auto close_observed = tests::run_awaitable(runtime, [&]() -> boost::asio::awaitable<support::Expected<void>> {
        co_await tests::wait_for(
                co_await boost::asio::this_coro::executor,
                [&] { return stub->close_calls() == before_close + 1; },
                std::chrono::seconds{2});
        co_return support::Expected<void>{};
    }());
    REQUIRE(close_observed.has_value());
    CHECK(stub->close_calls() == before_close + 1);
}

TEST_CASE("a 401 from the MCP server is an explicit re-login error", "[coding_agent][mcp][issue875][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto stub = std::make_shared<StubRequestAuth>("dummy-stale-token");
    auto client = connect_http_client(runtime, http_config(server), stub);
    auto response =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/unauthorized")));

    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().code == support::ErrorCode::OAuth);
    CHECK(response.error().message.find("re-authenticate") != std::string::npos);
    CHECK(response.error().message.find("mcp__echo") != std::string::npos);
    // The rejected credential never appears in the diagnostic.
    CHECK(response.error().message.find("dummy-stale-token") == std::string::npos);
    CHECK(response.error().detail.find("dummy-stale-token") == std::string::npos);
}
