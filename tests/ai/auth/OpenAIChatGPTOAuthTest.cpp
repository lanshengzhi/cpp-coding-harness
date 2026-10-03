#include "ai/auth/OpenAIChatGPTOAuth.hpp"
#include "support/FakeOAuthHttpClient.hpp"
#include "support/StreamAdapterFixture.hpp"
#include <cch/ai/Timestamps.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace cch;
using tests::FakeOAuthHttpClient;
using tests::run_async_result;
using tests::run_awaitable;

namespace {

constexpr std::string_view kTokenUrl = "https://auth.openai.com/api/accounts/oauth/token";
constexpr std::string_view kDeviceId = "123e4567-e89b-42d3-a456-426614174000";
constexpr std::string_view kIssuedClientId = "issued-client-id-abc";

ai::AuthInteraction manual_interaction(std::string pasted_url) {
    ai::AuthInteraction interaction;
    interaction.prompt =
            [pasted_url = std::move(pasted_url)](ai::AuthPrompt) -> support::AsyncResult<std::string> {
                return support::AsyncResult<std::string>(std::expected<std::string, support::Error>{pasted_url});
            };
    interaction.notify = [](const ai::AuthEvent&) {};
    return interaction;
}

std::string token_response_body() {
    return std::string{
                   "{\"access_token\":\"chatgpt-access\",\"refresh_token\":\"chatgpt-refresh\","
                   "\"expires_in\":3600,\"id_token\":\"dummy-id-token\","
                   "\"scope\":\"openid profile email offline_access resource.invoke chatgpt.tokens.use.direct\"}"};
}

} // namespace

TEST_CASE("ChatGPT OAuth login rejects a missing or malformed device ID",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    auto auth = ai::auth::make_openai_chatgpt_oauth_auth(http);

    SECTION("no options at all") {
        auto result = run_async_result(auth.login(
                manual_interaction("http://127.0.0.1:1455/auth/callback?code=x&state=y&client_id=z"),
                std::nullopt));
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("device ID") != std::string::npos);
    }
    SECTION("empty device ID") {
        ai::LoginOptions options;
        options.get_device_id = [] { return std::string{}; };
        auto result = run_async_result(auth.login(
                manual_interaction("http://127.0.0.1:1455/auth/callback?code=x&state=y&client_id=z"),
                std::move(options)));
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("device ID") != std::string::npos);
    }
    SECTION("non-UUID device ID") {
        ai::LoginOptions options;
        options.get_device_id = [] { return std::string{"not-a-uuid"}; };
        auto result = run_async_result(auth.login(
                manual_interaction("http://127.0.0.1:1455/auth/callback?code=x&state=y&client_id=z"),
                std::move(options)));
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("device ID") != std::string::npos);
    }
    CHECK(http->requests.empty());
}

TEST_CASE("ChatGPT OAuth refresh posts the frozen grant and preserves the issued client ID",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses[std::string{kTokenUrl}].push_back({200, token_response_body()});
    auto auth = ai::auth::make_openai_chatgpt_oauth_auth(http);

    ai::OAuthCredential credential;
    credential.refresh = "stored-refresh";
    credential.access = "stored-access";
    credential.expires = 1;
    credential.client_id = std::string{kIssuedClientId};

    auto result = run_async_result(auth.refresh(std::move(credential)));
    REQUIRE(result);
    CHECK(result->access == "chatgpt-access");
    CHECK(result->refresh == "chatgpt-refresh");
    REQUIRE(result->client_id);
    CHECK(*result->client_id == kIssuedClientId);
    // 3-minute expiry margin: expires = now + 3600s - 180s, inside a 10s window.
    const auto now = cch::ai::current_timestamp_ms();
    CHECK(result->expires > now + 3300 * 1000);
    CHECK(result->expires < now + 3600 * 1000);

    REQUIRE(http->requests.size() == 1);
    const auto& request = http->requests.front();
    CHECK(request.url == kTokenUrl);
    CHECK(request.headers.at("Content-Type") == "application/x-www-form-urlencoded");
    CHECK(request.body.find("grant_type=refresh_token") != std::string::npos);
    CHECK(request.body.find("client_id=issued-client-id-abc") != std::string::npos);
    CHECK(request.body.find("refresh_token=stored-refresh") != std::string::npos);
    CHECK(request.body.find("resource=https%3A%2F%2Fapi.openai.com%2Fv1") != std::string::npos);
    CHECK(request.body.find("code_verifier") == std::string::npos);
}

TEST_CASE("ChatGPT OAuth refresh fails without the issued client ID",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    auto auth = ai::auth::make_openai_chatgpt_oauth_auth(http);

    auto result = run_async_result(auth.refresh(ai::OAuthCredential{}));
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("reconnect ChatGPT") != std::string::npos);
    CHECK(http->requests.empty());
}

TEST_CASE("ChatGPT OAuth token response enforces the direct-token scope contract",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses[std::string{kTokenUrl}].push_back(
            {200,
                    "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":3600,"
                    "\"scope\":\"openid profile email\"}"});
    auto auth = ai::auth::make_openai_chatgpt_oauth_auth(http);

    ai::OAuthCredential credential;
    credential.refresh = "stored-refresh";
    credential.access = "stored-access";
    credential.expires = 1;
    credential.client_id = std::string{kIssuedClientId};

    auto result = run_async_result(auth.refresh(std::move(credential)));
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("chatgpt.tokens.use.direct") != std::string::npos);
}

TEST_CASE("ChatGPT OAuth token response rejects non-2xx with the response body",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    http->responses[std::string{kTokenUrl}].push_back({400, "{\"error\":\"invalid_grant\"}"});
    auto auth = ai::auth::make_openai_chatgpt_oauth_auth(http);

    ai::OAuthCredential credential;
    credential.refresh = "stored-refresh";
    credential.access = "stored-access";
    credential.expires = 1;
    credential.client_id = std::string{kIssuedClientId};

    auto result = run_async_result(auth.refresh(std::move(credential)));
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("(400)") != std::string::npos);
    CHECK(result.error().message.find("invalid_grant") != std::string::npos);
}

TEST_CASE("ChatGPT OAuth to_auth sends the access token as the API key",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    auto auth = ai::auth::make_openai_chatgpt_oauth_auth(http);

    ai::OAuthCredential credential;
    credential.access = "chatgpt-access";
    auto result = run_async_result(auth.to_auth(credential));
    REQUIRE(result);
    CHECK(result->api_key == "chatgpt-access");
    CHECK(result->headers.empty());
}

TEST_CASE("ChatGPT OAuth login broadcasts the frozen authorize URL contract",
        "[ai][auth][chatgpt][issue862][compat-pi]") {
    auto http = std::make_shared<FakeOAuthHttpClient>();
    // Port 1 is privileged/unused: the callback server degrades to
    // manual-input-only without touching the frozen 1455.
    auto impl = std::make_shared<ai::auth::OpenAIChatGPTOAuth>(
            http, ai::auth::OpenAIChatGPTOAuthOptions{.callback_host = "127.0.0.1", .callback_port = 1});

    ai::LoginOptions options;
    options.get_device_id = [] { return std::string{kDeviceId}; };

    std::optional<ai::AuthEvent> seen_url;
    std::stop_source stop;
    ai::AuthInteraction interaction;
    interaction.stop_token = stop.get_token();
    interaction.notify = [&seen_url](const ai::AuthEvent& event) {
        if (std::holds_alternative<ai::AuthUrl>(event.kind)) {
            seen_url = event;
        }
    };
    interaction.prompt = [&stop](ai::AuthPrompt) -> support::AsyncResult<std::string> {
        stop.request_stop();
        return support::AsyncResult<std::string>(
                std::unexpected(support::make_error(support::ErrorCode::Cancelled, "stop")));
    };

    auto result = run_awaitable(impl->login(std::move(interaction), std::move(options)));
    // Login fails (listen degraded + prompt cancelled), but the authorize URL
    // contract was broadcast before the exchange.
    REQUIRE_FALSE(result);
    REQUIRE(seen_url.has_value());
    const auto& url = std::get<ai::AuthUrl>(seen_url->kind);
    CHECK(url.url.starts_with("https://auth.openai.com/api/accounts/authorize?"));
    CHECK(url.url.find("client_id=dynamic_agent_client") != std::string::npos);
    CHECK(url.url.find("agent_name_hint=Pi") != std::string::npos);
    CHECK(url.url.find("ext_agent_host_id=urn%3Auuid%3A123e4567-e89b-42d3-a456-426614174000") !=
            std::string::npos);
    CHECK(url.url.find("response_type=code") != std::string::npos);
    CHECK(url.url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A1455%2Fauth%2Fcallback") != std::string::npos);
    CHECK(url.url.find("resource=https%3A%2F%2Fapi.openai.com%2Fv1") != std::string::npos);
    CHECK(url.url.find("scope=openid+profile+email+offline_access+resource.invoke+chatgpt.tokens.use.direct") !=
            std::string::npos);
    CHECK(url.url.find("code_challenge_method=S256") != std::string::npos);
    CHECK(url.instructions ==
            std::optional<std::string>{"Complete sign-in in your browser. If the callback does not complete, "
                                       "paste the final redirect URL here."});
}
