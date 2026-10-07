// Spec #865 fourth slice (#875), reworked for #884: MCP OAuth through the
// existing credential semantics. The provider reuses the shared `ai::auth`
// PKCE/callback helpers, the existing `AuthInteraction` presentation, and the
// existing `ai::OAuthCredential` contract; the state is stored in
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

#include "ai/JsonAccess.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/auth/Pkce.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpHttpClient.hpp"
#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"
#include "coding_agent/mcp/McpRequestAuthSource.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/Json.hpp"
#include "support/ReadyResult.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/Timestamps.hpp>
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

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;

namespace mcp = coding_agent::mcp;

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
    const auto* object = ai::json_object(fixture);
    REQUIRE(object != nullptr);
    const auto* member = ai::json_object_member(*object, key);
    REQUIRE(member != nullptr);
    return *member;
}

[[nodiscard]] std::string fixture_string(const support::JsonValue::object_t& object, std::string_view key) {
    const auto value = ai::json_string_member(object, key);
    REQUIRE(value.has_value());
    return std::string{*value};
}

[[nodiscard]] mcp::McpOAuthServerConfig server_config_from_fixture(const support::JsonValue& fixture) {
    const auto& server = fixture_object(fixture, "server");
    return mcp::McpOAuthServerConfig{
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
    const auto pairs = ai::auth::parse_query_pairs(url.substr(query_start + 1));
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
    std::shared_ptr<mcp::McpAuthStore> store;
    std::string server_name{"echo"};
    std::string server_url{"https://echo.example.com/mcp"};
    std::optional<std::string> auth_url{std::nullopt};
    std::vector<ai::AuthEvent> events{};
    std::vector<ai::AuthPrompt> prompts{};
    bool manual_prompt_cancelled{false};

    /// Runs the loop to completion. `on_auth_url` (if set) drives the loopback
    /// callback once the authorization URL is observed.
    [[nodiscard]] support::Expected<void> run(const mcp::McpOAuthServerConfig& config,
            std::move_only_function<boost::asio::awaitable<void>(std::string)> on_auth_url) {
        boost::asio::io_context io;
        auto executor = io.get_executor();
        tests::EnvVarGuard callback_host_guard{"PI_OAUTH_CALLBACK_HOST"};
        callback_host_guard.unset();

        ai::AuthInteraction interaction;
        interaction.notify = [this](const ai::AuthEvent& event) {
            events.push_back(event);
            if (const auto* url = std::get_if<ai::AuthUrl>(&event.kind)) {
                auth_url = url->url;
            }
        };
        interaction.prompt = [this, executor](ai::AuthPrompt prompt) -> support::AsyncResult<std::string> {
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
                                     const ai::AuthEvent& event) mutable {
            if (original_notify) {
                original_notify(event);
            }
            if (const auto* url = std::get_if<ai::AuthUrl>(&event.kind)) {
                url_seen->try_send(boost::system::error_code{}, url->url);
            }
        };

        auto provider = std::make_shared<mcp::McpOAuthProvider>(config, http);
        auto store = this->store;
        auto server_name = this->server_name;
        auto server_url = this->server_url;
        auto login_future = boost::asio::co_spawn(
                io,
                [provider, store, server_name, server_url, interaction = std::move(interaction)]() mutable
                        -> boost::asio::awaitable<support::ExpectedVoid> {
                    co_return co_await support::detail::await_async_result(
                            mcp::login_mcp_server(store, server_name, server_url, *provider, std::move(interaction)));
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

[[nodiscard]] mcp::McpOAuthState oauth_state(std::string access, std::string refresh, std::int64_t expires) {
    mcp::McpOAuthState state;
    state.server_url = "https://echo.example.com/mcp";
    mcp::McpOAuthTokens tokens;
    tokens.access_token = std::move(access);
    tokens.token_type = "Bearer";
    if (!refresh.empty()) {
        tokens.refresh_token = std::move(refresh);
    }
    state.tokens = std::move(tokens);
    state.tokens_expire_at = expires;
    state.client_information = mcp::McpOAuthClientInformation{"pike-mcp-test-client", std::nullopt};
    return state;
}

void seed_state(mcp::McpAuthStore& store, const mcp::McpOAuthState& state) {
    auto stored = store.save("echo", state.server_url, state);
    REQUIRE(stored.has_value());
}

[[nodiscard]] std::optional<mcp::McpOAuthState> read_state(mcp::McpAuthStore& store) {
    auto stored = store.load("echo", "https://echo.example.com/mcp");
    REQUIRE(stored.has_value());
    return std::move(*stored);
}

/// Everything but `access` — the part a refresh must preserve or rotate.

// ── a no-credential / per-request auth source for the transport seam ─────────

class StubRequestAuth final : public mcp::McpRequestAuthSource {
public:
    explicit StubRequestAuth(std::string token) : token_(std::move(token)) {}

    [[nodiscard]] support::AsyncResult<std::map<std::string, std::string>> current_headers() override {
        ++calls_;
        return tests::ready_result(std::map<std::string, std::string>{{"Authorization", "Bearer " + token_}});
    }

    [[nodiscard]] int calls() const noexcept { return calls_; }

private:
    std::string token_;
    int calls_{0};
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
    HttpFixtureServer() {
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

[[nodiscard]] mcp::McpHttpServerConfig http_config(const HttpFixtureServer& server) {
    mcp::McpHttpServerConfig config;
    config.name = "echo";
    config.url = server.url();
    return config;
}

[[nodiscard]] std::shared_ptr<mcp::McpHttpClient> connect_http_client(tests::RuntimeFixture& runtime,
        mcp::McpHttpServerConfig config,
        std::shared_ptr<mcp::McpRequestAuthSource> request_auth = nullptr) {
    auto client = tests::run_awaitable(runtime,
            mcp::McpHttpClient::connect(std::move(config),
                    std::make_shared<ai::providers::BoostBeastStreamTransport>(),
                    std::move(request_auth)));
    REQUIRE(client.has_value());
    return *client;
}

[[nodiscard]] std::string authorization_header(
        tests::RuntimeFixture& runtime, const std::shared_ptr<mcp::McpHttpClient>& client) {
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
    harness.store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");

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
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");

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
    auto removed = runtime.run(mcp::logout_mcp_server(store, "echo", "https://echo.example.com/mcp"));
    REQUIRE(removed.has_value());
    CHECK_FALSE(read_state(*store).has_value());
}

TEST_CASE("a valid stored MCP OAuth credential resolves the bearer token without a token request",
        "[coding_agent][mcp][issue875][spec]") {
    const auto config = server_config_from_fixture(read_replay_fixture());
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("dummy-access-token",
                    "dummy-refresh-token",
                    ai::current_timestamp_ms() + std::chrono::hours{24}.count() * 1000));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    auto provider = std::make_shared<mcp::McpOAuthProvider>(config, http);
    auto resolver =
            std::make_shared<mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE(headers.has_value());
    REQUIRE(headers->contains("Authorization"));
    CHECK(headers->at("Authorization") == "Bearer dummy-access-token");
    CHECK(http->requests.empty());
}

TEST_CASE("a rotated MCP OAuth refresh is persisted before the request continues",
        "[coding_agent][mcp][issue875][spec]") {
    const auto fixture = read_replay_fixture();
    const auto config = server_config_from_fixture(fixture);
    const auto& refresh_exchange = fixture_object(fixture, "refresh_exchange");
    const std::string refresh_body = fixture_string(fixture_object(refresh_exchange, "response"), "body");

    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    // Expiring within the five-minute margin, so the request-time path refreshes.
    seed_state(*store, oauth_state("dummy-old-access-token", "dummy-refresh-token", ai::current_timestamp_ms() + 1000));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {{200, refresh_body}};
    auto provider = std::make_shared<mcp::McpOAuthProvider>(config, http);
    auto resolver =
            std::make_shared<mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

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
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");
    seed_state(*store,
            oauth_state("dummy-old-access-token", "dummy-dead-refresh-token", ai::current_timestamp_ms() + 1000));

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    http->responses[config.token_url] = {
            {static_cast<int>(*ai::json_integer_member(response, "status")), fixture_string(response, "body")}};
    auto provider = std::make_shared<mcp::McpOAuthProvider>(config, http);
    auto resolver =
            std::make_shared<mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

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
    auto store = std::make_shared<mcp::McpAuthStore>(workspace.path() / "mcp-auth.json");

    auto http = std::make_shared<tests::FakeOAuthHttpClient>();
    auto provider = std::make_shared<mcp::McpOAuthProvider>(config, http);
    auto resolver =
            std::make_shared<mcp::McpOAuthTokenResolver>(store, "echo", "https://echo.example.com/mcp", provider);

    auto headers = tests::run_awaitable(runtime, support::detail::await_async_result(resolver->current_headers()));
    REQUIRE_FALSE(headers.has_value());
    CHECK(headers.error().code == support::ErrorCode::OAuth);
    CHECK(headers.error().message.find("re-authenticate") != std::string::npos);
    CHECK(http->requests.empty());
}

TEST_CASE("the MCP HTTP transport attaches a credential only when the server needs one",
        "[coding_agent][mcp][issue875][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    // No request-auth source: no credentials are attached, and no implicit
    // sign-in is attempted.
    auto anonymous = connect_http_client(runtime, http_config(server));
    CHECK(authorization_header(runtime, anonymous).empty());

    // With a request-auth source the token is attached, and it is re-resolved
    // for every request (never cached on the connection).
    auto stub = std::make_shared<StubRequestAuth>("dummy-access-token");
    auto authenticated = connect_http_client(runtime, http_config(server), stub);
    const int before = stub->calls();
    CHECK(authorization_header(runtime, authenticated) == "Bearer dummy-access-token");
    CHECK(authorization_header(runtime, authenticated) == "Bearer dummy-access-token");
    CHECK(stub->calls() == before + 2);
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
