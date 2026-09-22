#include "ai/auth/OpenRouterOAuth.hpp"
#include "ai/auth/Pkce.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/ReadyResult.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using tests::FakeOAuthHttpClient;
using tests::run_async_result;

namespace {

constexpr std::string_view kTokenUrl = "https://openrouter.ai/api/v1/auth/keys";

struct CallbackEndpoint {
    std::string host{};
    std::uint16_t port{0};
    std::string path{};
};

CallbackEndpoint callback_endpoint(std::string_view callback_url) {
    const auto authority_start = callback_url.find("://");
    const auto host_start = authority_start == std::string_view::npos ? 0 : authority_start + 3;
    const auto path_start = callback_url.find('/', host_start);
    const auto authority = callback_url.substr(host_start,
            path_start == std::string_view::npos ? callback_url.size() - host_start : path_start - host_start);
    const auto port_start = authority.rfind(':');
    CallbackEndpoint endpoint;
    endpoint.host = std::string{authority.substr(0, port_start)};
    const auto port_text = authority.substr(port_start + 1);
    std::from_chars(port_text.data(), port_text.data() + port_text.size(), endpoint.port);
    endpoint.path = path_start == std::string_view::npos ? "/" : std::string{callback_url.substr(path_start)};
    return endpoint;
}

std::string query_param(std::string_view url, std::string_view key) {
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return {};
    }
    const auto pairs = ai::auth::parse_query_pairs(url.substr(query_start + 1));
    const auto found = pairs.find(std::string{key});
    return found == pairs.end() ? std::string{} : found->second;
}

boost::asio::awaitable<std::pair<int, std::string>> http_get(
        const std::string& host, std::uint16_t port, const std::string& target) {
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
    co_return std::pair{
            static_cast<int>(response.result_int()),
            std::move(response.body()),
    };
}

using SignalChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

struct LoginHarness {
    std::shared_ptr<FakeOAuthHttpClient> http = std::make_shared<FakeOAuthHttpClient>();
    ai::auth::OpenRouterOAuthOptions options{};
    std::stop_token login_stop_token{};
    std::optional<std::string> auth_url{std::nullopt};
    std::vector<ai::AuthEvent> events{};
    std::vector<ai::AuthPrompt> prompts{};
    bool manual_prompt_cancelled{false};
    std::optional<std::pair<int, std::string>> callback_response{std::nullopt};
    std::optional<std::pair<int, std::string>> duplicate_response{std::nullopt};
    std::optional<std::pair<int, std::string>> first_response{std::nullopt};

    std::function<ai::AuthPromptHook(boost::asio::any_io_executor)> prompt_factory;

    [[nodiscard]] support::Expected<ai::OAuthCredential> run(
            std::function<boost::asio::awaitable<void>(const std::string&)> on_auth_url = nullptr) {
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
        interaction.prompt = prompt_factory ? prompt_factory(executor)
                                            : [this](ai::AuthPrompt prompt) -> support::AsyncResult<std::string> {
            prompts.push_back(prompt);
            return tests::ready_result<std::string>("unused");
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

        auto login_future = boost::asio::co_spawn(
                io,
                [this, &interaction]() -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                    auto auth = ai::auth::make_openrouter_oauth_auth(http, options);
                    co_return co_await support::detail::await_async_result(auth.login(std::move(interaction)));
                },
                boost::asio::use_future);

        if (on_auth_url) {
            boost::asio::co_spawn(
                    io,
                    [url_seen, on_auth_url = std::move(on_auth_url)]() -> boost::asio::awaitable<void> {
                        std::string url;
                        boost::system::error_code receive_error;
                        url = co_await url_seen->async_receive(
                                boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
                        if (!receive_error) {
                            co_await on_auth_url(url);
                        }
                    },
                    boost::asio::detached);
        }

        io.run();
        return login_future.get();
    }
};

ai::AuthPromptHook pending_manual_prompt(LoginHarness& harness, boost::asio::any_io_executor executor) {
    auto cancelled = std::make_shared<SignalChannel>(executor, 1);
    return [&harness, cancelled](ai::AuthPrompt prompt) -> support::AsyncResult<std::string> {
        return support::detail::make_async_result([&harness, cancelled, prompt = std::move(prompt)]() mutable
                                                          -> boost::asio::awaitable<support::Expected<std::string>> {
            harness.prompts.push_back(prompt);
            if (!prompt.stop_token) {
                co_return std::unexpected(
                        support::make_error(support::ErrorCode::OAuth, "missing manual prompt stop token"));
            }
            std::stop_callback callback{
                    *prompt.stop_token,
                    [&harness, cancelled] {
                        harness.manual_prompt_cancelled = true;
                        cancelled->try_send(boost::system::error_code{});
                    },
            };
            boost::system::error_code receive_error;
            co_await cancelled->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, receive_error));
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Login cancelled"));
        });
    };
}

void use_pending_prompt(LoginHarness& harness) {
    harness.prompt_factory = [&harness](boost::asio::any_io_executor executor) {
        return pending_manual_prompt(harness, executor);
    };
}

} // namespace

TEST_CASE(
        "OpenRouter OAuth completes PKCE callback authorization and returns an API key", "[ai][auth][issue764][spec]") {
    LoginHarness harness;
    harness.http->responses[std::string{kTokenUrl}] = {
            {200, R"({"key":"sk-or-test"})"},
    };
    use_pending_prompt(harness);

    auto result = harness.run([&harness](const std::string& url) -> boost::asio::awaitable<void> {
        const auto callback = callback_endpoint(query_param(url, "callback_url"));
        harness.callback_response =
                co_await http_get(callback.host, callback.port, callback.path + "?code=authorization-code");
    });

    REQUIRE(result);
    CHECK(result->access == "sk-or-test");
    CHECK(result->refresh.empty());
    CHECK(result->expires == 9007199254740991LL);
    CHECK_FALSE(result->account_id.has_value());
    CHECK(harness.manual_prompt_cancelled);
    REQUIRE(harness.auth_url);
    CHECK(harness.auth_url->starts_with("https://openrouter.ai/auth?"));
    CHECK(query_param(*harness.auth_url, "code_challenge_method") == "S256");

    const auto callback = callback_endpoint(query_param(*harness.auth_url, "callback_url"));
    CHECK(callback.host == "127.0.0.1");
    CHECK(callback.port != 0);
    CHECK(callback.path.starts_with("/oauth/callback/"));
    CHECK(callback.path.size() > std::string_view{"/oauth/callback/"}.size());

    REQUIRE(harness.http->requests.size() == 1);
    const auto& request = harness.http->requests.front();
    CHECK(request.url == kTokenUrl);
    const auto content_type = request.headers.find("Content-Type");
    REQUIRE(content_type != request.headers.end());
    CHECK(content_type->second == "application/json");
    const auto accept = request.headers.find("Accept");
    REQUIRE(accept != request.headers.end());
    CHECK(accept->second == "application/json");
    auto body = support::read_json(request.body);
    REQUIRE(body);
    const auto* object = ai::json_object(*body);
    REQUIRE(object != nullptr);
    const auto code = ai::json_string_member(*object, "code");
    REQUIRE(code);
    CHECK(*code == "authorization-code");
    const auto method = ai::json_string_member(*object, "code_challenge_method");
    REQUIRE(method);
    CHECK(*method == "S256");
    const auto verifier = ai::json_string_member(*object, "code_verifier");
    REQUIRE(verifier);

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    REQUIRE(EVP_Digest(verifier->data(), verifier->size(), digest.data(), &digest_length, EVP_sha256(), nullptr) == 1);
    CHECK(ai::auth::base64url_encode(std::string_view{reinterpret_cast<const char*>(digest.data()), digest_length}) ==
            query_param(*harness.auth_url, "code_challenge"));
    REQUIRE(harness.callback_response);
    CHECK(harness.callback_response->first == 200);
    CHECK(harness.callback_response->second.find("account authorization") != std::string::npos);
    for (const auto& event : harness.events) {
        if (const auto* auth_url = std::get_if<ai::AuthUrl>(&event.kind)) {
            CHECK(auth_url->url.find("sk-or-test") == std::string::npos);
        }
        if (const auto* progress = std::get_if<ai::AuthProgress>(&event.kind)) {
            CHECK(progress->message.find("permanent") == std::string::npos);
            CHECK(progress->message.find("subscription") == std::string::npos);
        }
    }
}

TEST_CASE("OpenRouter OAuth reports exchange failures through the callback page", "[ai][auth][issue764][spec]") {
    SECTION("non-success response") {
        LoginHarness harness;
        harness.http->responses[std::string{kTokenUrl}] = {
                {403, R"({"error":{"message":"invalid authorization"}})"},
        };
        use_pending_prompt(harness);

        auto result = harness.run([&harness](const std::string& url) -> boost::asio::awaitable<void> {
            const auto callback = callback_endpoint(query_param(url, "callback_url"));
            harness.callback_response =
                    co_await http_get(callback.host, callback.port, callback.path + "?code=bad-code");
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().message == "OpenRouter OAuth key exchange failed (HTTP 403): invalid authorization");
        REQUIRE(harness.callback_response);
        CHECK(harness.callback_response->first == 502);
        CHECK(harness.callback_response->second.find("invalid authorization") != std::string::npos);
    }

    SECTION("successful response without a key") {
        LoginHarness harness;
        harness.http->responses[std::string{kTokenUrl}] = {
                {200, R"({"user_id":"user-1"})"},
        };
        use_pending_prompt(harness);

        auto result = harness.run([&harness](const std::string& url) -> boost::asio::awaitable<void> {
            const auto callback = callback_endpoint(query_param(url, "callback_url"));
            harness.callback_response =
                    co_await http_get(callback.host, callback.port, callback.path + "?code=missing-key");
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().message == "OpenRouter OAuth response carries no \"key\"");
        REQUIRE(harness.callback_response);
        CHECK(harness.callback_response->first == 502);
    }
}

TEST_CASE("OpenRouter OAuth claims one callback before exchanging its code", "[ai][auth][issue764][spec]") {
    LoginHarness harness;
    harness.http->responses[std::string{kTokenUrl}] = {
            {200, R"({"key":"sk-or-duplicate"})"},
    };
    harness.http->respond_delay = std::chrono::milliseconds{100};
    use_pending_prompt(harness);

    auto result = harness.run([&harness](const std::string& url) -> boost::asio::awaitable<void> {
        const auto callback = callback_endpoint(query_param(url, "callback_url"));
        const auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::co_spawn(
                executor,
                [&harness, callback]() -> boost::asio::awaitable<void> {
                    harness.first_response =
                            co_await http_get(callback.host, callback.port, callback.path + "?code=first-code");
                },
                boost::asio::detached);

        boost::asio::steady_timer timer(executor);
        while (harness.http->requests.empty()) {
            timer.expires_after(std::chrono::milliseconds{1});
            boost::system::error_code error;
            co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        harness.duplicate_response =
                co_await http_get(callback.host, callback.port, callback.path + "?code=second-code");
    });

    REQUIRE(result);
    CHECK(result->access == "sk-or-duplicate");
    REQUIRE(harness.first_response);
    CHECK(harness.first_response->first == 200);
    REQUIRE(harness.duplicate_response);
    CHECK(harness.duplicate_response->first == 409);
    CHECK(harness.http->requests.size() == 1);
}

TEST_CASE("OpenRouter OAuth accepts a manual redirect URL and bare code", "[ai][auth][issue764][spec]") {
    SECTION("redirect URL") {
        LoginHarness harness;
        harness.http->responses[std::string{kTokenUrl}] = {
                {200, R"({"key":"sk-or-manual-url"})"},
        };
        harness.prompt_factory = [&harness](boost::asio::any_io_executor) {
            return [&harness](ai::AuthPrompt prompt) -> support::AsyncResult<std::string> {
                harness.prompts.push_back(prompt);
                const auto* manual = std::get_if<ai::AuthPromptManualCode>(&prompt.kind);
                REQUIRE(manual != nullptr);
                REQUIRE(manual->placeholder);
                return tests::ready_result<std::string>(*manual->placeholder + "?code=manual-code");
            };
        };

        auto result = harness.run();
        REQUIRE(result);
        CHECK(result->access == "sk-or-manual-url");
        REQUIRE(harness.http->requests.size() == 1);
    }

    SECTION("bare code") {
        LoginHarness harness;
        harness.http->responses[std::string{kTokenUrl}] = {
                {200, R"({"key":"sk-or-manual-code"})"},
        };
        harness.prompt_factory = [&harness](boost::asio::any_io_executor) {
            return [&harness](ai::AuthPrompt prompt) -> support::AsyncResult<std::string> {
                harness.prompts.push_back(prompt);
                return tests::ready_result<std::string>("manual-code");
            };
        };

        auto result = harness.run();
        REQUIRE(result);
        CHECK(result->access == "sk-or-manual-code");
        REQUIRE(harness.http->requests.size() == 1);
    }
}

TEST_CASE("OpenRouter OAuth respects the callback host override", "[ai][auth][issue764][spec]") {
    tests::EnvVarGuard callback_host("PI_OAUTH_CALLBACK_HOST", "127.0.0.2");
    LoginHarness harness;
    harness.http->responses[std::string{kTokenUrl}] = {
            {200, R"({"key":"sk-or-host"})"},
    };
    use_pending_prompt(harness);

    auto result = harness.run([&harness](const std::string& url) -> boost::asio::awaitable<void> {
        const auto callback = callback_endpoint(query_param(url, "callback_url"));
        harness.callback_response = co_await http_get(callback.host, callback.port, callback.path + "?code=host-code");
    });

    REQUIRE(result);
    CHECK(result->access == "sk-or-host");
    CHECK(callback_endpoint(query_param(*harness.auth_url, "callback_url")).host == "127.0.0.2");
}

TEST_CASE("OpenRouter OAuth cancellation avoids events and exchange traffic", "[ai][auth][issue764][spec]") {
    SECTION("already cancelled") {
        std::stop_source stop;
        stop.request_stop();
        LoginHarness harness;
        harness.login_stop_token = stop.get_token();

        auto result = harness.run();
        REQUIRE_FALSE(result);
        CHECK(result.error().code == support::ErrorCode::Cancelled);
        CHECK(result.error().message == "Login cancelled");
        CHECK(harness.events.empty());
        CHECK(harness.http->requests.empty());
    }

    SECTION("cancelled while waiting") {
        auto stop = std::make_shared<std::stop_source>();
        LoginHarness harness;
        harness.login_stop_token = stop->get_token();
        use_pending_prompt(harness);

        auto result = harness.run([stop](const std::string&) -> boost::asio::awaitable<void> {
            stop->request_stop();
            co_return;
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().code == support::ErrorCode::Cancelled);
        CHECK(result.error().message == "Login cancelled");
        CHECK(harness.http->requests.empty());
    }
}

TEST_CASE("OpenRouter OAuth refresh is local and request auth uses the API key", "[ai][auth][issue764][spec]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    auto auth = ai::auth::make_openrouter_oauth_auth(http);
    const ai::OAuthCredential credential{
            .refresh = {},
            .access = "sk-or-local",
            .expires = 9007199254740991LL,
    };

    auto refreshed = run_async_result(auth.refresh(credential));
    REQUIRE(refreshed);
    CHECK(*refreshed == credential);
    CHECK(http->requests.empty());

    auto request_auth = run_async_result(auth.to_auth(credential));
    REQUIRE(request_auth);
    CHECK(request_auth->api_key == "sk-or-local");
    CHECK(request_auth->headers.empty());
}
