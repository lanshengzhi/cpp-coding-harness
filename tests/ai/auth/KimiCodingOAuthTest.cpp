#include "ai/auth/DevicePoll.hpp"
#include "ai/auth/KimiCodingOAuth.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/auth/Pkce.hpp"
#include "support/EnvVarGuard.hpp"
#include "util/ExpectedMacros.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

template <typename T>
T run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    auto result = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
    io.run();
    return result.get();
}

std::string query_param(const std::string& text, const std::string& key) {
    const auto query_start = text.find('?');
    const auto start = query_start == std::string::npos ? 0 : query_start + 1;
    const auto pairs = ai::auth::parse_query_pairs(
        std::string_view{text}.substr(start));
    const auto found = pairs.find(key);
    return found == pairs.end() ? std::string{} : found->second;
}

std::string device_authorization_json() {
    return R"({"user_code":"ABCD-1234","device_code":"device-code-123",)"
           R"("verification_uri":"https://www.kimi.com/code",)"
           R"("verification_uri_complete":"https://www.kimi.com/code?user_code=ABCD-1234",)"
           R"("interval":1,"expires_in":600})";
}

std::string token_json() {
    return R"({"access_token":"dummy-access-token","refresh_token":"dummy-refresh-token","expires_in":3600})";
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

    boost::asio::awaitable<util::Expected<ai::auth::OAuthHttpResponse>> post(
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) override {
        requests.push_back(Request{
            url,
            std::move(headers),
            std::move(body),
            stop_token,
        });
        if (fail_first_n_requests > 0) {
            --fail_first_n_requests;
            if (stop_token.stop_requested()) {
                co_return std::unexpected(util::make_error(
                    util::ErrorCode::Cancelled,
                    "fake client cancelled"));
            }
            co_return std::unexpected(failure_error.value_or(util::make_error(
                util::ErrorCode::Network,
                "connection reset")));
        }
        if (respond_delay > std::chrono::milliseconds::zero()) {
            // Emulate a real transport that observes the composed request stop
            // token while a request is in flight.
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor, respond_delay);
            boost::system::error_code error;
            co_await timer.async_wait(boost::asio::redirect_error(
                boost::asio::use_awaitable, error));
            if (stop_token.stop_requested()) {
                co_return std::unexpected(util::make_error(
                    util::ErrorCode::Cancelled,
                    "fake client cancelled"));
            }
        }
        auto& queue = responses[requests.back().url];
        if (queue.empty()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Network,
                "no scripted response for " + requests.back().url));
        }
        auto scripted = std::move(queue.front());
        queue.pop_front();
        if (stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "fake client cancelled"));
        }
        co_return ai::auth::OAuthHttpResponse{
            .status_code = scripted.status,
            .body = std::move(scripted.body),
        };
    }

    std::map<std::string, std::deque<ScriptedResponse>, std::less<>> responses;
    std::vector<Request> requests;
    std::optional<util::Error> failure_error;
    int fail_first_n_requests{0};
    std::chrono::milliseconds respond_delay{0};
};

ai::AuthInteraction make_interaction(
    std::vector<ai::AuthEvent>* events,
    std::stop_token stop_token = {}) {
    ai::AuthInteraction interaction;
    interaction.stop_token = stop_token;
    interaction.notify = [events](const ai::AuthEvent& event) {
        if (events != nullptr) {
            events->push_back(event);
        }
    };
    return interaction;
}

ai::OAuthAuth make_auth(
    const std::shared_ptr<FakeOAuthHttpClient>& http,
    ai::auth::KimiCodingOAuthOptions options = {}) {
    return ai::auth::make_kimi_coding_oauth_auth(http, std::move(options));
}

} // namespace

TEST_CASE("Kimi login runs the RFC 8628 device flow with the frozen notify content", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {400, R"({"error":"authorization_pending"})"},
        {200, token_json()},
    };

    std::vector<ai::AuthEvent> events;
    auto auth = make_auth(http);
    auto result = run_awaitable(auth.login(make_interaction(&events)));

    REQUIRE(result);
    CHECK(result->access == "dummy-access-token");
    CHECK(result->refresh == "dummy-refresh-token");
    CHECK(result->expires > 0);
    CHECK(!result->account_id.has_value());

    // The device_code event carries verification_uri_complete, interval, and
    // expires, in pi's order and shape.
    REQUIRE(events.size() == 1);
    const auto* device = std::get_if<ai::AuthDeviceCode>(&events.front().kind);
    REQUIRE(device != nullptr);
    CHECK(device->user_code == "ABCD-1234");
    CHECK(device->verification_uri ==
          "https://www.kimi.com/code?user_code=ABCD-1234");
    CHECK(device->interval_seconds == 1);
    CHECK(device->expires_in_seconds == 600);

    // Request shapes: form bodies with the frozen client id.
    REQUIRE(http->requests.size() == 3);
    CHECK(http->requests[0].url ==
          "https://auth.kimi.com/api/oauth/device_authorization");
    CHECK(query_param(http->requests[0].body, "client_id") ==
          "17e5f671-d194-4dfb-9706-5516cb48c098");
    const auto& poll = http->requests[1];
    CHECK(poll.url == "https://auth.kimi.com/api/oauth/token");
    CHECK(query_param(poll.body, "client_id") ==
          "17e5f671-d194-4dfb-9706-5516cb48c098");
    CHECK(query_param(poll.body, "device_code") == "device-code-123");
    CHECK(query_param(poll.body, "grant_type") ==
          "urn:ietf:params:oauth:grant-type:device_code");
    const auto content_type = poll.headers.find("Content-Type");
    REQUIRE(content_type != poll.headers.end());
    CHECK(content_type->second == "application/x-www-form-urlencoded");
    const auto accept = poll.headers.find("Accept");
    REQUIRE(accept != poll.headers.end());
    CHECK(accept->second == "application/json");
}

TEST_CASE("Kimi login waits for the interval before the first poll", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {200, token_json()},
    };
    auto auth = make_auth(http);

    auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable(auth.login(make_interaction(nullptr)));
    REQUIRE(result);

    // wait_before_first_poll: the first poll arrives only after the 1s
    // interval (RFC 8628 section 3.2 default; here the server's interval=1).
    REQUIRE(http->requests.size() == 2);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds{900});
}

TEST_CASE("Kimi login applies interval and expires defaults of 5s and 15min", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, R"({"user_code":"U","device_code":"D",)"
               R"("verification_uri":"https://www.kimi.com/code",)"
               R"("verification_uri_complete":"https://www.kimi.com/code?user_code=U"})"},
    };
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {200, token_json()},
    };

    std::vector<ai::AuthEvent> events;
    auto auth = make_auth(http);
    auto result = run_awaitable(auth.login(make_interaction(&events)));
    REQUIRE(result);

    REQUIRE(events.size() == 1);
    const auto* device = std::get_if<ai::AuthDeviceCode>(&events.front().kind);
    REQUIRE(device != nullptr);
    CHECK(device->interval_seconds == 5);
    CHECK(device->expires_in_seconds == 15 * 60);
}

TEST_CASE("Kimi login rejects a non-http(s) verification_uri_complete", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, R"({"user_code":"ABCD-1234","device_code":"device-code-123",)"
               R"("verification_uri":"https://www.kimi.com/code",)"
               R"j("verification_uri_complete":"javascript:alert(1)")j"},
    };
    auto auth = make_auth(http);

    auto result = run_awaitable(auth.login(make_interaction(nullptr)));

    REQUIRE(!result);
    CHECK(result.error().code == util::ErrorCode::OAuth);
    CHECK(result.error().message.find(
              "Invalid Kimi Code device authorization response") !=
          std::string::npos);
    CHECK(http->requests.size() == 1);
}

TEST_CASE("Kimi login fails when the device code expires and when denied", "[ai][auth][issue344]") {
    auto expired_http = std::make_shared<FakeOAuthHttpClient>();
    expired_http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    expired_http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {400, R"({"error":"expired_token"})"},
    };
    auto expired_auth = make_auth(expired_http);
    auto expired = run_awaitable(expired_auth.login(make_interaction(nullptr)));
    REQUIRE(!expired);
    CHECK(expired.error().message ==
          "Kimi Code device authorization expired. Please restart login.");

    auto denied_http = std::make_shared<FakeOAuthHttpClient>();
    denied_http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    denied_http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {400, R"({"error":"access_denied"})"},
    };
    auto denied_auth = make_auth(denied_http);
    auto denied = run_awaitable(denied_auth.login(make_interaction(nullptr)));
    REQUIRE(!denied);
    CHECK(denied.error().message == "Kimi Code login was denied.");
}

TEST_CASE("Kimi login honors the KIMI_CODE_OAUTH_HOST override with trailing slash stripped", "[ai][auth][issue344]") {
    tests::EnvVarGuard host("KIMI_CODE_OAUTH_HOST", "https://auth.example.com/");
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.example.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    http->responses["https://auth.example.com/api/oauth/token"] = {
        {200, token_json()},
    };
    auto auth = make_auth(http);

    auto result = run_awaitable(auth.login(make_interaction(nullptr)));
    REQUIRE(result);
    CHECK(http->requests.front().url ==
          "https://auth.example.com/api/oauth/device_authorization");
}

TEST_CASE("Kimi login cancellation normalizes to Login cancelled", "[ai][auth][issue344]") {
    std::stop_source login_stop;
    login_stop.request_stop();
    auto http = std::make_shared<FakeOAuthHttpClient>();
    auto auth = make_auth(http);

    auto result = run_awaitable(
        auth.login(make_interaction(nullptr, login_stop.get_token())));

    REQUIRE(!result);
    CHECK(result.error().code == util::ErrorCode::Cancelled);
    CHECK(result.error().message == "Login cancelled");
}

TEST_CASE("Kimi login honors a slow_down server interval and then completes", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {400, R"({"error":"slow_down","interval":1})"},
        {200, token_json()},
    };
    auto auth = make_auth(http);

    auto result = run_awaitable(auth.login(make_interaction(nullptr)));

    REQUIRE(result);
    CHECK(result->access == "dummy-access-token");
    // device_authorization + two token polls (slow_down then complete).
    REQUIRE(http->requests.size() == 3);
}

TEST_CASE("device poll helper surfaces the frozen slow_down timeout message verbatim", "[ai][auth][issue344]") {
    // Reaching the deadline with one or more slow_down responses must fail with
    // pi's frozen WSL/VM clock-drift message, byte for byte.
    auto result = run_awaitable(ai::auth::poll_device_flow<int>(
        ai::auth::DevicePollOptions<int>{
            .interval_seconds = 1,
            .expires_in_seconds = 1,
            .wait_before_first_poll = false,
            .poll = []()
                -> boost::asio::awaitable<util::Expected<ai::auth::DevicePollResult<int>>> {
                co_return ai::auth::DevicePollResult<int>{
                    .kind = ai::auth::DevicePollResult<int>::SlowDown{},
                };
            },
        }));
    REQUIRE(!result);
    CHECK(result.error().message ==
          "Device flow timed out after one or more slow_down responses. This is "
          "often caused by clock drift in WSL or VM environments. Please sync or "
          "restart the VM clock and try again.");
}

TEST_CASE("Kimi per-request timeout composes with the login cancellation token", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/device_authorization"] = {
        {200, device_authorization_json()},
    };
    // Emulate a transport stuck for 500ms; the injected 40ms request timeout
    // fires first and stops the composed token.
    http->respond_delay = std::chrono::milliseconds{500};
    auto auth = make_auth(
        http,
        ai::auth::KimiCodingOAuthOptions{
            .request_timeout = std::chrono::milliseconds{40},
        });

    auto result = run_awaitable(auth.login(make_interaction(nullptr)));

    REQUIRE(!result);
    CHECK(result.error().code == util::ErrorCode::Timeout);
    CHECK(result.error().message == "Kimi Code OAuth request timed out");
}

TEST_CASE("Kimi refresh rotates the credential and toAuth derives the Bearer header", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {200, R"({"access_token":"new-access","refresh_token":"new-refresh","expires_in":3600})"},
    };
    auto auth = make_auth(http);

    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto result = run_awaitable(auth.refresh(ai::OAuthCredential{
        .refresh = "old-refresh",
        .access = "old-access",
        .expires = 0,
    }));
    REQUIRE(result);
    CHECK(result->access == "new-access");
    CHECK(result->refresh == "new-refresh");
    CHECK(result->expires >= before + 3600 * 1000);

    REQUIRE(http->requests.size() == 1);
    const auto& request = http->requests[0];
    CHECK(request.url == "https://auth.kimi.com/api/oauth/token");
    CHECK(query_param(request.body, "grant_type") == "refresh_token");
    CHECK(query_param(request.body, "refresh_token") == "old-refresh");
    CHECK(query_param(request.body, "client_id") ==
          "17e5f671-d194-4dfb-9706-5516cb48c098");

    auto request_auth = run_awaitable(auth.to_auth(*result));
    REQUIRE(request_auth);
    REQUIRE(request_auth->headers.size() == 1);
    CHECK(request_auth->headers.at("Authorization") == "Bearer new-access");
}

TEST_CASE("Kimi refresh retries 429 with exponential backoff and succeeds", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {429, R"({"error":"temporarily_unavailable"})"},
        {200, token_json()},
    };
    auto auth = make_auth(
        http,
        ai::auth::KimiCodingOAuthOptions{
            .refresh_backoff_base = std::chrono::milliseconds{10},
        });

    const auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable(auth.refresh(ai::OAuthCredential{
        .refresh = "old",
        .access = "old",
        .expires = 0,
    }));
    REQUIRE(result);
    CHECK(http->requests.size() == 2);
    // attempt 1 waits refresh_backoff_base (10ms) before retrying.
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds{9});
}

TEST_CASE("Kimi refresh fails unauthorized immediately on invalid_grant", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {400, R"({"error":"invalid_grant","error_description":"bad token"})"},
    };
    auto auth = make_auth(http);

    auto result = run_awaitable(auth.refresh(ai::OAuthCredential{
        .refresh = "old",
        .access = "old",
        .expires = 0,
    }));

    REQUIRE(!result);
    CHECK(result.error().message ==
          "Kimi Code token refresh unauthorized (status 400): bad token");
    // Unauthorized failures are not retried.
    CHECK(http->requests.size() == 1);
}

TEST_CASE("Kimi refresh fails unauthorized immediately on 401 and 403", "[ai][auth][issue344]") {
    for (const int status : {401, 403}) {
        auto http = std::make_shared<FakeOAuthHttpClient>();
        http->responses["https://auth.kimi.com/api/oauth/token"] = {
            {status, R"({"error":"unauthorized"})"},
        };
        auto auth = make_auth(http);
        auto result = run_awaitable(auth.refresh(ai::OAuthCredential{
            .refresh = "old",
            .access = "old",
            .expires = 0,
        }));
        REQUIRE(!result);
        CHECK(result.error().message.find(
                  "Kimi Code token refresh unauthorized (status " +
                      std::to_string(status)) !=
              std::string::npos);
        CHECK(http->requests.size() == 1);
    }
}

TEST_CASE("Kimi refresh gives up after the retry ceiling on persistent 5xx", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {500, R"({"error":"oops"})"},
        {500, R"({"error":"oops"})"},
        {500, R"({"error":"oops"})"},
        {500, R"({"error":"oops"})"},
    };
    auto auth = make_auth(
        http,
        ai::auth::KimiCodingOAuthOptions{
            .refresh_backoff_base = std::chrono::milliseconds{2},
        });

    auto result = run_awaitable(auth.refresh(ai::OAuthCredential{
        .refresh = "old",
        .access = "old",
        .expires = 0,
    }));

    REQUIRE(!result);
    CHECK(result.error().code == util::ErrorCode::OAuth);
    CHECK(result.error().message.find("Kimi Code token refresh failed") !=
          std::string::npos);
    // attempts 0..3 inclusive = 4 requests total.
    CHECK(http->requests.size() == 4);
}

TEST_CASE("Kimi refresh retries transport failures up to the retry ceiling", "[ai][auth][issue344]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->fail_first_n_requests = 3;
    http->failure_error = util::make_error(
        util::ErrorCode::Network, "connection reset");
    http->responses["https://auth.kimi.com/api/oauth/token"] = {
        {200, token_json()},
    };
    auto auth = make_auth(
        http,
        ai::auth::KimiCodingOAuthOptions{
            .refresh_backoff_base = std::chrono::milliseconds{2},
        });

    auto result = run_awaitable(auth.refresh(ai::OAuthCredential{
        .refresh = "old",
        .access = "old",
        .expires = 0,
    }));
    REQUIRE(result);
    // three transport failures are retried, the fourth attempt succeeds.
    CHECK(http->requests.size() == 4);
}
