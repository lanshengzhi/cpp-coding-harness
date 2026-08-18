#include "ai/auth/DevicePoll.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/auth/OAuthCallbackServer.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/auth/OpenAICodexOAuth.hpp"
#include "ai/auth/OauthPage.hpp"
#include "ai/auth/Pkce.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PiFixture.hpp"
#include "support/ExpectedMacros.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace cch;

namespace {

template <typename T>
T run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    auto result = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
    io.run();
    return result.get();
}

template <typename T, typename E>
std::expected<T, E> run_async_result(cch::support::AsyncResult<T, E> result) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
        io,
        [](cch::support::AsyncResult<T, E> op) -> boost::asio::awaitable<std::expected<T, E>> {
            co_return co_await cch::ai::detail::await_async_result(std::move(op));
        }(std::move(result)),
        boost::asio::use_future);
    io.run();
    return future.get();
}

std::string access_token_for(const std::string& account_id) {
    const auto header = ai::auth::base64url_encode(R"({"alg":"none"})");
    const auto payload = ai::auth::base64url_encode(
        "{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"" +
        account_id + "\"}}");
    return header + "." + payload + ".signature";
}

std::string token_response_json(const std::string& account_id) {
    return "{\"access_token\":\"" + access_token_for(account_id) +
        "\",\"refresh_token\":\"dummy-refresh-token\",\"expires_in\":3600}";
}

std::uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io);
    boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::make_address("127.0.0.1"), 0);
    acceptor.open(endpoint.protocol());
    acceptor.bind(endpoint);
    const auto port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

boost::asio::awaitable<std::pair<int, std::string>> http_get(
    const std::string& host,
    std::uint16_t port,
    const std::string& target) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    using tcp = asio::ip::tcp;

    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(
        tcp::endpoint(asio::ip::make_address(host), port),
        asio::use_awaitable);
    http::request<http::string_body> request{http::verb::get, target, 11};
    request.set(http::field::host, host);
    co_await http::async_write(socket, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(socket, buffer, response, asio::use_awaitable);
    socket.close();
    co_return std::pair{static_cast<int>(response.result_int()), response.body()};
}

std::string query_param(const std::string& text, const std::string& key) {
    // Works for both `?key=value` URLs and bare `key=value&...` form bodies.
    const auto query_start = text.find('?');
    const auto start = query_start == std::string::npos ? 0 : query_start + 1;
    const auto pairs = ai::auth::parse_query_pairs(
        std::string_view{text}.substr(start));
    const auto found = pairs.find(key);
    return found == pairs.end() ? std::string{} : found->second;
}

class FakeOAuthHttpClient final : public ai::auth::OAuthHttpClient {
public:
    struct Request {
        std::string url;
        std::map<std::string, std::string, std::less<>> headers;
        std::string body;
        std::stop_token stop_token;
    };
    struct ScriptedResponse {
        int status{200};
        std::string body;
    };

    boost::asio::awaitable<support::Expected<ai::auth::OAuthHttpResponse>> post(
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) override {
        requests.push_back(Request{
            std::move(url),
            std::move(headers),
            std::move(body),
            stop_token,
        });
        if (failure) {
            if (stop_token.stop_requested()) {
                co_return std::unexpected(support::make_error(
                    support::ErrorCode::Cancelled,
                    "fake client cancelled"));
            }
            co_return std::unexpected(*failure);
        }
        auto& queue = responses[requests.back().url];
        if (queue.empty()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Network,
                "no scripted response for " + requests.back().url));
        }
        auto scripted = std::move(queue.front());
        queue.pop_front();
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "fake client cancelled"));
        }
        co_return ai::auth::OAuthHttpResponse{
            .status_code = scripted.status,
            .body = std::move(scripted.body),
        };
    }

    std::map<std::string, std::deque<ScriptedResponse>, std::less<>> responses;
    std::vector<Request> requests;
    std::optional<support::Error> failure;
};

using UrlSeenChannel = boost::asio::experimental::channel<
    void(boost::system::error_code, std::string)>;
using SignalChannel = boost::asio::experimental::channel<
    void(boost::system::error_code)>;

support::Expected<std::string> select_browser_prompt(const ai::AuthPrompt& prompt) {
    if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
        return std::string{"browser"};
    }
    if (std::holds_alternative<ai::AuthPromptManualCode>(prompt.kind)) {
        return std::string{"dummy-manual-code"};
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::OAuth,
        "unexpected prompt"));
}

/// Runs one login flow on a single io_context, firing an optional async
/// callback the moment the `auth_url` event arrives so a test can drive the
/// callback-vs-manual-code race deterministically.
struct BrowserLoginHarness {
    std::shared_ptr<FakeOAuthHttpClient> http =
        std::make_shared<FakeOAuthHttpClient>();
    ai::auth::OpenAICodexOAuthOptions options{};
    std::stop_token login_stop_token{};
    std::optional<std::string> auth_url;
    std::vector<ai::AuthEvent> events;
    std::vector<ai::AuthPrompt> prompts;

    /// Builds the prompt hook; the io executor is supplied at run time so
    /// hooks can create asio channels on the live executor.
    std::function<ai::AuthPromptHook(boost::asio::any_io_executor)> make_prompt_hook;

    [[nodiscard]] ai::AuthPromptHook sync_prompt_hook(
        std::function<support::Expected<std::string>(const ai::AuthPrompt&)> sync) {
        return [this, sync = std::move(sync)](ai::AuthPrompt prompt)
        -> cch::support::AsyncResult<std::string> {
        return cch::ai::detail::make_async_result(
            [this, sync = std::move(sync), prompt = std::move(prompt)]() mutable
                -> boost::asio::awaitable<support::Expected<std::string>> {

                prompts.push_back(prompt);
                co_return sync(prompt);

        });
    };
    }

    support::Expected<ai::OAuthCredential> run(
        std::function<boost::asio::awaitable<void>(const std::string&)> on_auth_url =
            nullptr) {
        boost::asio::io_context io;
        auto executor = io.get_executor();

        ai::AuthInteraction interaction;
        interaction.stop_token = login_stop_token;
        interaction.notify = [this](const ai::AuthEvent& event) {
            events.push_back(event);
            if (const auto* url = std::get_if<ai::AuthUrl>(&event.kind)) {
                auth_url = url->url;
            }
        };
        interaction.prompt = make_prompt_hook
            ? make_prompt_hook(executor)
            : sync_prompt_hook([](const ai::AuthPrompt& prompt) {
                  return select_browser_prompt(prompt);
              });

        auto url_seen = std::make_shared<UrlSeenChannel>(executor, 1);
        auto original_notify = std::move(interaction.notify);
        interaction.notify =
            [url_seen, original_notify = std::move(original_notify)](
                const ai::AuthEvent& event) mutable {
                if (original_notify) {
                    original_notify(event);
                }
                if (const auto* url = std::get_if<ai::AuthUrl>(&event.kind)) {
                    url_seen->try_send(boost::system::error_code{}, url->url);
                }
            };

        auto login_future = boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                auto auth = ai::auth::make_openai_codex_oauth_auth(http, options);
                co_return co_await cch::ai::detail::await_async_result(
                    auth.login(std::move(interaction)));
            },
            boost::asio::use_future);
        if (on_auth_url) {
            boost::asio::co_spawn(
                io,
                [url_seen, on_auth_url = std::move(on_auth_url)]()
                    -> boost::asio::awaitable<void> {
                    std::string url;
                    boost::system::error_code receive_error;
                    url = co_await url_seen->async_receive(
                        boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                    if (receive_error) {
                        co_return;
                    }
                    co_await on_auth_url(url);
                },
                boost::asio::detached);
        }
        io.run();
        return login_future.get();
    }
};

} // namespace

TEST_CASE("PKCE S256 challenge is the base64url SHA-256 of the verifier", "[ai][auth][issue343]") {
    auto pkce = ai::auth::generate_pkce();
    REQUIRE(pkce);
    CHECK(pkce->verifier.size() == 43); // 32 bytes -> 43 base64url chars
    CHECK(pkce->challenge.size() == 43);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    REQUIRE(EVP_Digest(
                pkce->verifier.data(),
                pkce->verifier.size(),
                digest.data(),
                &digest_length,
                EVP_sha256(),
                nullptr) == 1);
    const std::string expected = ai::auth::base64url_encode(std::string_view{
        reinterpret_cast<const char*>(digest.data()),
        digest_length,
    });
    CHECK(pkce->challenge == expected);
    CHECK(pkce->verifier.find('+') == std::string::npos);
    CHECK(pkce->verifier.find('/') == std::string::npos);
    CHECK(pkce->verifier.find('=') == std::string::npos);
}

TEST_CASE("OAuth state is 16 random bytes as 32 hex characters", "[ai][auth][issue343]") {
    auto first = ai::auth::create_oauth_state();
    auto second = ai::auth::create_oauth_state();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->size() == 32);
    CHECK(*first != *second);
    const auto hex = [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    };
    CHECK(std::all_of(first->begin(), first->end(), hex));
}

TEST_CASE("parseAuthorizationInput accepts URL, code#state, query and bare code", "[ai][auth][issue343]") {
    const auto url = ai::auth::parse_authorization_input(
        "http://localhost:1455/auth/callback?code=a%20b&state=s1");
    CHECK(url.code == std::string{"a b"});
    CHECK(url.state == std::string{"s1"});

    const auto fragment = ai::auth::parse_authorization_input("code123#state456");
    CHECK(fragment.code == std::string{"code123"});
    CHECK(fragment.state == std::string{"state456"});

    const auto query = ai::auth::parse_authorization_input("code=abc&state=def");
    CHECK(query.code == std::string{"abc"});
    CHECK(query.state == std::string{"def"});

    const auto bare = ai::auth::parse_authorization_input("  bare-code  ");
    CHECK(bare.code == std::string{"bare-code"});
    CHECK(!bare.state.has_value());

    const auto empty = ai::auth::parse_authorization_input("");
    CHECK(!empty.code.has_value());
    CHECK(!empty.state.has_value());

    const auto url_without_code = ai::auth::parse_authorization_input("http://x/?state=s");
    CHECK(!url_without_code.code.has_value());
    CHECK(url_without_code.state == std::string{"s"});
}

TEST_CASE("extractAccountId reads the unverified JWT chatgpt_account_id claim", "[ai][auth][issue343]") {
    const auto token = access_token_for("account-123");
    const auto account_id = ai::auth::extract_account_id(token);
    REQUIRE(account_id);
    CHECK(*account_id == "account-123");
}

TEST_CASE("extractAccountId fails on malformed or claim-less JWTs", "[ai][auth][issue343]") {
    CHECK(!ai::auth::extract_account_id("not-a-jwt").has_value());
    CHECK(!ai::auth::extract_account_id("a.b").has_value());
    const auto no_claim = ai::auth::base64url_encode(R"({"sub":"x"})");
    CHECK(!ai::auth::extract_account_id("a." + no_claim + ".c").has_value());
    const auto empty_id = ai::auth::base64url_encode(
        R"({"https://api.openai.com/auth":{"chatgpt_account_id":""}})");
    CHECK(!ai::auth::extract_account_id("a." + empty_id + ".c").has_value());
}

TEST_CASE("OAuth HTML pages match the frozen pi output verbatim", "[ai][auth][issue343]") {
    const auto success = tests::read_pi_fixture_text("auth/oauth-success-callback.html");
    const auto not_found = tests::read_pi_fixture_text("auth/oauth-error-route-not-found.html");
    const auto state = tests::read_pi_fixture_text("auth/oauth-error-state-mismatch.html");
    const auto code = tests::read_pi_fixture_text("auth/oauth-error-missing-code.html");
    const auto internal = tests::read_pi_fixture_text("auth/oauth-error-internal.html");
    REQUIRE(success);
    REQUIRE(not_found);
    REQUIRE(state);
    REQUIRE(code);
    REQUIRE(internal);
    CHECK(ai::auth::oauth_success_html(
              "OpenAI authentication completed. You can close this window.") == *success);
    CHECK(ai::auth::oauth_error_html("Callback route not found.") == *not_found);
    CHECK(ai::auth::oauth_error_html("State mismatch.") == *state);
    CHECK(ai::auth::oauth_error_html("Missing authorization code.") == *code);
    CHECK(ai::auth::oauth_error_html(
              "Internal error while processing OAuth callback.") == *internal);
}

TEST_CASE("browser login succeeds through the callback server and cancels the prompt", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.options.callback_host = "127.0.0.1";
    harness.options.callback_port = free_port();
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-cb")},
    };

    auto stop_observed = std::make_shared<bool>(false);
    harness.make_prompt_hook = [&harness, stop_observed](
                                   boost::asio::any_io_executor executor)
        -> ai::AuthPromptHook {
        auto cancel_seen = std::make_shared<SignalChannel>(executor, 1);
        return [&harness, stop_observed, cancel_seen](ai::AuthPrompt prompt)
        -> cch::support::AsyncResult<std::string> {
        return cch::ai::detail::make_async_result(
            [&harness, stop_observed, cancel_seen, prompt = std::move(prompt)]() mutable
                -> boost::asio::awaitable<support::Expected<std::string>> {

                harness.prompts.push_back(prompt);
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    co_return std::string{"browser"};
                }
                REQUIRE(std::holds_alternative<ai::AuthPromptManualCode>(prompt.kind));
                REQUIRE(prompt.stop_token.has_value());
                // The callback win must cancel this prompt via the per-prompt token
                // (pi's manualAbort). Await that cancellation, then reject.
                std::stop_callback callback{
                    *prompt.stop_token,
                    [stop_observed, cancel_seen] {
                        *stop_observed = true;
                        cancel_seen->try_send(boost::system::error_code{});
                    },
                };
                boost::system::error_code receive_error;
                co_await cancel_seen->async_receive(
                    boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                co_return std::unexpected(support::make_error(
                    support::ErrorCode::Cancelled,
                    "Login cancelled"));

        });
    };
    };

    std::string fired_state;
    auto result = harness.run(
        [&harness, &fired_state](const std::string& url)
            -> boost::asio::awaitable<void> {
            fired_state = query_param(url, "state");
            const auto response = co_await http_get(
                "127.0.0.1",
                harness.options.callback_port,
                "/auth/callback?code=cb-code&state=" + fired_state);
            const auto expected =
                tests::read_pi_fixture_text("auth/oauth-success-callback.html");
            REQUIRE(expected);
            CHECK(response.first == 200);
            CHECK(response.second == *expected);
        });

    REQUIRE(result);
    CHECK(result->account_id == std::string{"account-cb"});
    CHECK(result->access == access_token_for("account-cb"));
    CHECK(result->refresh == "dummy-refresh-token");
    // Callback win cancelled the manual prompt via its per-prompt token.
    CHECK(*stop_observed);

    // Frozen prompt and event content.
    REQUIRE(harness.prompts.size() == 2);
    const auto* select = std::get_if<ai::AuthPromptSelect>(&harness.prompts[0].kind);
    REQUIRE(select != nullptr);
    CHECK(select->message == "Select OpenAI Codex login method:");
    REQUIRE(select->options.size() == 2);
    CHECK(select->options[0].id == "browser");
    CHECK(select->options[0].label == "Browser login (default)");
    CHECK(select->options[1].id == "device_code");
    CHECK(select->options[1].label == "Device code login (headless)");
    const auto* manual = std::get_if<ai::AuthPromptManualCode>(&harness.prompts[1].kind);
    REQUIRE(manual != nullptr);
    CHECK(manual->message ==
          "Complete login in your browser, or paste the authorization code / redirect URL here:");
    CHECK(manual->placeholder == std::string{"http://localhost:1455/auth/callback"});
    REQUIRE(harness.events.size() == 1);
    const auto* auth_url = std::get_if<ai::AuthUrl>(&harness.events[0].kind);
    REQUIRE(auth_url != nullptr);
    CHECK(auth_url->instructions == "A browser window should open. Complete login to finish.");

    // The authorize URL carries pi's exact originator and PKCE params.
    const auto& url = *harness.auth_url;
    CHECK(url.starts_with(
        "https://auth.openai.com/oauth/authorize?response_type=code"
        "&client_id=app_EMoamEEZ73f0CkXaXp7hrann"
        "&redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"
        "&scope=openid+profile+email+offline_access"));
    CHECK(query_param(url, "code_challenge_method") == "S256");
    CHECK(query_param(url, "id_token_add_organizations") == "true");
    CHECK(query_param(url, "codex_cli_simplified_flow") == "true");
    CHECK(query_param(url, "originator") == "pi");
    CHECK(query_param(url, "state") == fired_state);

    // PKCE: the challenge in the URL equals base64url(SHA256(verifier)) of the
    // code_verifier actually sent in the exchange; the token request keeps
    // pi's frozen redirect_uri even though the test server binds a free port.
    REQUIRE(harness.http->requests.size() == 1);
    const auto& exchange = harness.http->requests[0];
    CHECK(exchange.url == "https://auth.openai.com/oauth/token");
    const auto verifier = query_param(exchange.body, "code_verifier");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    REQUIRE(EVP_Digest(
                verifier.data(), verifier.size(), digest.data(),
                &digest_length, EVP_sha256(), nullptr) == 1);
    const auto expected_challenge = ai::auth::base64url_encode(std::string_view{
        reinterpret_cast<const char*>(digest.data()), digest_length});
    CHECK(expected_challenge == query_param(url, "code_challenge"));
    CHECK(query_param(exchange.body, "grant_type") == "authorization_code");
    CHECK(query_param(exchange.body, "client_id") == "app_EMoamEEZ73f0CkXaXp7hrann");
    CHECK(query_param(exchange.body, "code") == "cb-code");
    CHECK(query_param(exchange.body, "redirect_uri") ==
          "http://localhost:1455/auth/callback");
    const auto content_type = exchange.headers.find("Content-Type");
    REQUIRE(content_type != exchange.headers.end());
    CHECK(content_type->second == "application/x-www-form-urlencoded");
}

TEST_CASE("PI_OAUTH_CALLBACK_HOST overrides the callback server bind host", "[ai][auth][issue343]") {
    tests::EnvVarGuard callback_host("PI_OAUTH_CALLBACK_HOST", "127.0.0.2");

    BrowserLoginHarness harness;
    harness.options.callback_host = std::nullopt; // env override applies
    harness.options.callback_port = free_port();
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-envhost")},
    };

    auto stop_observed = std::make_shared<bool>(false);
    harness.make_prompt_hook = [&harness, stop_observed](
                                   boost::asio::any_io_executor executor)
        -> ai::AuthPromptHook {
        auto cancel_seen = std::make_shared<SignalChannel>(executor, 1);
        return [&harness, stop_observed, cancel_seen](ai::AuthPrompt prompt)
        -> cch::support::AsyncResult<std::string> {
        return cch::ai::detail::make_async_result(
            [&harness, stop_observed, cancel_seen, prompt = std::move(prompt)]() mutable
                -> boost::asio::awaitable<support::Expected<std::string>> {

                harness.prompts.push_back(prompt);
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    co_return std::string{"browser"};
                }
                std::stop_callback callback{
                    *prompt.stop_token,
                    [stop_observed, cancel_seen] {
                        *stop_observed = true;
                        cancel_seen->try_send(boost::system::error_code{});
                    },
                };
                boost::system::error_code receive_error;
                co_await cancel_seen->async_receive(
                    boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                co_return std::unexpected(support::make_error(
                    support::ErrorCode::Cancelled,
                    "Login cancelled"));

        });
    };
    };

    std::string fired_state;
    auto result = harness.run(
        [&harness, &fired_state](const std::string& url)
            -> boost::asio::awaitable<void> {
            fired_state = query_param(url, "state");
            const auto response = co_await http_get(
                "127.0.0.2",
                harness.options.callback_port,
                "/auth/callback?code=env-host-code&state=" + fired_state);
            CHECK(response.first == 200);
        });

    REQUIRE(result);
    CHECK(result->account_id == std::string{"account-envhost"});
    CHECK(*stop_observed);
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(query_param(harness.http->requests[0].body, "code") == "env-host-code");
}

TEST_CASE("manual code wins the race and closes the callback wait", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.options.callback_host = "127.0.0.1";
    harness.options.callback_port = free_port();
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-manual")},
    };

    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [&harness](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"browser"};
                }
                // Manual win: paste the full redirect URL including the state
                // captured from the earlier auth_url event.
                const auto& url = *harness.auth_url;
                return "http://localhost:1455/auth/callback?code=manual-code&state=" +
                    query_param(url, "state");
            });
    };

    auto result = harness.run();
    REQUIRE(result);
    CHECK(result->account_id == std::string{"account-manual"});
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(query_param(harness.http->requests[0].body, "code") == "manual-code");
}

TEST_CASE("manual code accepts a bare code with no state", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.options.callback_host = "127.0.0.1";
    harness.options.callback_port = free_port();
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-bare")},
    };

    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"browser"};
                }
                return std::string{"bare-code"};
            });
    };

    auto result = harness.run();
    REQUIRE(result);
    CHECK(result->account_id == std::string{"account-bare"});
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(query_param(harness.http->requests[0].body, "code") == "bare-code");
}

TEST_CASE("manual code with a mismatched state fails with State mismatch", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.options.callback_host = "127.0.0.1";
    harness.options.callback_port = free_port();

    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"browser"};
                }
                return std::string{"code-value#wrong-state"};
            });
    };

    auto result = harness.run();
    REQUIRE(!result);
    CHECK(result.error().message == "State mismatch");
    CHECK(harness.http->requests.empty());
}

TEST_CASE("callback server rejects wrong path, state, and missing code in pi order", "[ai][auth][issue343]") {
    boost::asio::io_context io;
    boost::asio::co_spawn(
        io,
        []() -> boost::asio::awaitable<void> {
            auto started = co_await ai::auth::OAuthCallbackServer::start(
                ai::auth::OAuthCallbackServerOptions{
                    .host = "127.0.0.1",
                    .port = free_port(),
                    .state = "expected-state",
                });
            REQUIRE(started);
            auto server = *started;
            const auto port = server->bound_port();

            const auto not_found = co_await http_get("127.0.0.1", port, "/other");
            CHECK(not_found.first == 404);
            CHECK(not_found.second ==
                  ai::auth::oauth_error_html("Callback route not found."));

            const auto wrong_state = co_await http_get(
                "127.0.0.1", port, "/auth/callback?code=c&state=wrong");
            CHECK(wrong_state.first == 400);
            CHECK(wrong_state.second ==
                  ai::auth::oauth_error_html("State mismatch."));

            const auto no_state = co_await http_get(
                "127.0.0.1", port, "/auth/callback?code=c");
            CHECK(no_state.first == 400);
            CHECK(no_state.second ==
                  ai::auth::oauth_error_html("State mismatch."));

            const auto no_code = co_await http_get(
                "127.0.0.1", port, "/auth/callback?state=expected-state");
            CHECK(no_code.first == 400);
            CHECK(no_code.second ==
                  ai::auth::oauth_error_html("Missing authorization code."));

            server->close();
        },
        boost::asio::detached);
    io.run();
}

TEST_CASE("callback server settles the wait only for a valid callback", "[ai][auth][issue343]") {
    boost::asio::io_context io;
    boost::asio::co_spawn(
        io,
        [&io]() -> boost::asio::awaitable<void> {
            auto started = co_await ai::auth::OAuthCallbackServer::start(
                ai::auth::OAuthCallbackServerOptions{
                    .host = "127.0.0.1",
                    .port = free_port(),
                    .state = "expected-state",
                });
            REQUIRE(started);
            auto server = *started;
            const auto port = server->bound_port();

            // A rejected request (bad state) must not settle the wait.
            const auto invalid = co_await http_get(
                "127.0.0.1", port, "/auth/callback?code=c&state=bad");
            CHECK(invalid.first == 400);

            auto code_seen =
                std::make_shared<SignalChannel>(io.get_executor(), 1);
            auto code_value = std::make_shared<std::optional<std::string>>();
            boost::asio::co_spawn(
                io.get_executor(),
                [server, code_seen, code_value]()
                    -> boost::asio::awaitable<void> {
                    auto code = co_await server->wait_for_code();
                    REQUIRE(code);
                    *code_value = std::move(*code);
                    code_seen->try_send(boost::system::error_code{});
                },
                boost::asio::detached);

            const auto response = co_await http_get(
                "127.0.0.1", port,
                "/auth/callback?code=real-code&state=expected-state");
            CHECK(response.first == 200);

            boost::system::error_code receive_error;
            co_await code_seen->async_receive(
                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
            CHECK(*code_value == std::string{"real-code"});
            server->close();
        },
        boost::asio::detached);
    io.run();
}

TEST_CASE("listen failure degrades to manual code entry only", "[ai][auth][issue343]") {
    // Occupy a port so the callback server cannot bind it.
    boost::asio::io_context blocker_io;
    boost::asio::ip::tcp::acceptor blocker(blocker_io);
    boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::make_address("127.0.0.1"), 0);
    blocker.open(endpoint.protocol());
    blocker.bind(endpoint);
    blocker.listen();
    const auto occupied = blocker.local_endpoint().port();

    BrowserLoginHarness harness;
    harness.options.callback_host = "127.0.0.1";
    harness.options.callback_port = occupied;
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-degraded")},
    };

    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"browser"};
                }
                return std::string{"manual-only-code"};
            });
    };

    auto result = harness.run();
    REQUIRE(result);
    CHECK(result->account_id == std::string{"account-degraded"});
    REQUIRE(harness.http->requests.size() == 1);
    CHECK(query_param(harness.http->requests[0].body, "code") == "manual-only-code");
}

TEST_CASE("browser login cancellation normalizes to Login cancelled", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.options.callback_host = "127.0.0.1";
    harness.options.callback_port = free_port();
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-cancelled")},
    };

    std::stop_source login_stop;
    login_stop.request_stop();
    harness.login_stop_token = login_stop.get_token();
    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"browser"};
                }
                return std::string{"cancelled-code"};
            });
    };

    auto result = harness.run();
    REQUIRE(!result);
    CHECK(result.error().code == support::ErrorCode::Cancelled);
    CHECK(result.error().message == "Login cancelled");
}

TEST_CASE("unknown Codex login method fails", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"telepathy"};
                }
                return std::string{"unused"};
            });
    };
    auto result = harness.run();
    REQUIRE(!result);
    CHECK(result.error().message == "Unknown OpenAI Codex login method: telepathy");
}

TEST_CASE("device code login offers frozen content and completes", "[ai][auth][issue343]") {
    BrowserLoginHarness harness;
    harness.http->responses["https://auth.openai.com/api/accounts/deviceauth/usercode"] = {
        {200, R"({"device_auth_id":"device-auth-id","user_code":"ABCD-1234","interval":"5"})"},
    };
    harness.http->responses["https://auth.openai.com/api/accounts/deviceauth/token"] = {
        {200, R"({"authorization_code":"oauth-code","code_verifier":"device-code-verifier"})"},
    };
    harness.http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-device")},
    };

    harness.make_prompt_hook = [&harness](boost::asio::any_io_executor)
        -> ai::AuthPromptHook {
        return harness.sync_prompt_hook(
            [](const ai::AuthPrompt& prompt) -> support::Expected<std::string> {
                if (std::holds_alternative<ai::AuthPromptSelect>(prompt.kind)) {
                    return std::string{"device_code"};
                }
                return std::unexpected(support::make_error(
                    support::ErrorCode::OAuth,
                    "unexpected prompt"));
            });
    };

    auto result = harness.run();
    REQUIRE(result);
    CHECK(result->account_id == std::string{"account-device"});

    REQUIRE(harness.events.size() == 1);
    const auto* device = std::get_if<ai::AuthDeviceCode>(&harness.events[0].kind);
    REQUIRE(device != nullptr);
    CHECK(device->user_code == "ABCD-1234");
    CHECK(device->verification_uri == "https://auth.openai.com/codex/device");
    CHECK(device->interval_seconds == 5);
    CHECK(device->expires_in_seconds == 900);

    REQUIRE(harness.http->requests.size() == 3);
    CHECK(harness.http->requests[0].body.find(
              R"("client_id":"app_EMoamEEZ73f0CkXaXp7hrann")") !=
          std::string::npos);
    CHECK(harness.http->requests[1].body.find("\"device_auth_id\":\"device-auth-id\"") !=
          std::string::npos);
    const auto& exchange = harness.http->requests[2];
    CHECK(query_param(exchange.body, "grant_type") == "authorization_code");
    CHECK(query_param(exchange.body, "code") == "oauth-code");
    CHECK(query_param(exchange.body, "code_verifier") == "device-code-verifier");
    CHECK(query_param(exchange.body, "redirect_uri") ==
          "https://auth.openai.com/deviceauth/callback");
}

TEST_CASE("Codex refresh succeeds and rotates the credential", "[ai][auth][issue343]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.openai.com/oauth/token"] = {
        {200, token_response_json("account-refreshed")},
    };
    auto auth = ai::auth::make_openai_codex_oauth_auth(http);

    auto result = run_async_result(auth.refresh(ai::OAuthCredential{
        .refresh = "dummy-old-refresh",
        .access = "dummy-old-access",
        .expires = 0,
    }));
    REQUIRE(result);
    CHECK(result->access == access_token_for("account-refreshed"));
    CHECK(result->account_id == std::string{"account-refreshed"});
    REQUIRE(http->requests.size() == 1);
    CHECK(query_param(http->requests[0].body, "grant_type") == "refresh_token");
    CHECK(query_param(http->requests[0].body, "refresh_token") == "dummy-old-refresh");
    CHECK(query_param(http->requests[0].body, "client_id") ==
          "app_EMoamEEZ73f0CkXaXp7hrann");
}

TEST_CASE("Codex refresh failure carries the status and body without stderr output", "[ai][auth][issue343]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.openai.com/oauth/token"] = {
        {401,
         R"({"error":{"message":"Could not validate your token. Please try signing in again.","type":"invalid_request_error"}})"},
    };
    auto auth = ai::auth::make_openai_codex_oauth_auth(http);

    auto result = run_async_result(auth.refresh(ai::OAuthCredential{
        .refresh = "invalid-refresh-token",
        .access = "invalid-access-token",
        .expires = 0,
    }));
    REQUIRE(!result);
    CHECK(result.error().message.find("OpenAI Codex token refresh failed (401)") !=
          std::string::npos);
    CHECK(result.error().message.find("Could not validate your token") !=
          std::string::npos);
}

TEST_CASE("Codex toAuth derives the access token as the API key", "[ai][auth][issue343]") {
    auto auth = ai::auth::make_openai_codex_oauth_auth(nullptr);
    auto result = run_async_result(auth.to_auth(ai::OAuthCredential{
        .refresh = "r",
        .access = "access-token-value",
        .expires = 1,
        .account_id = "account-x",
    }));
    REQUIRE(result);
    CHECK(result->api_key == std::string{"access-token-value"});
}

TEST_CASE("device poll helper honors pending, complete, and cancellation", "[ai][auth][issue343]") {
    std::stop_source cancel;

    auto pending_then_complete = [&]() -> boost::asio::awaitable<void> {
        auto polled = std::make_shared<int>(0);
        auto result = co_await ai::auth::poll_device_flow<int>(
            ai::auth::DevicePollOptions<int>{
                .interval_seconds = 1,
                .expires_in_seconds = 30,
                .poll = [polled]() -> boost::asio::awaitable<support::Expected<ai::auth::DevicePollResult<int>>> {
                    ++*polled;
                    if (*polled == 1) {
                        co_return ai::auth::DevicePollResult<int>{
                            .kind = ai::auth::DevicePollResult<int>::Pending{},
                        };
                    }
                    co_return ai::auth::DevicePollResult<int>{
                        .kind = ai::auth::DevicePollResult<int>::Complete{.value = 42},
                    };
                },
                .stop_token = cancel.get_token(),
            });
        REQUIRE(result);
        CHECK(*result == 42);
        CHECK(*polled == 2);
    };
    run_awaitable(pending_then_complete());

    auto cancelled = [&]() -> boost::asio::awaitable<void> {
        cancel.request_stop();
        auto result = co_await ai::auth::poll_device_flow<int>(
            ai::auth::DevicePollOptions<int>{
                .interval_seconds = 5,
                .expires_in_seconds = 30,
                .poll = []() -> boost::asio::awaitable<support::Expected<ai::auth::DevicePollResult<int>>> {
                    co_return ai::auth::DevicePollResult<int>{
                        .kind = ai::auth::DevicePollResult<int>::Pending{},
                    };
                },
                .stop_token = cancel.get_token(),
            });
        REQUIRE(!result);
        CHECK(result.error().code == support::ErrorCode::Cancelled);
        CHECK(result.error().message == "Login cancelled");
    };
    run_awaitable(cancelled());
}
