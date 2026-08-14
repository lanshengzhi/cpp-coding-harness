#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "ai/auth/KimiCodingOAuth.hpp"
#include "ai/auth/OAuthHttpClient.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/providers/KimiProvider.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
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

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

[[nodiscard]] std::string read_fixture(std::string_view name) {
    const auto path = std::filesystem::path{CCH_SOURCE_DIR} /
                      "fixtures/pi-ai/auth-storage" / name;
    auto text = read_text(path);
    if (text.ends_with('\n')) {
        text.pop_back();
    }
    return text;
}

/// The persisted `expires` is a live wall-clock timestamp; the goldens pin the
/// deterministic structure with a `0` sentinel. Replace the digits after every
/// `"expires":` with `0` before comparing.
[[nodiscard]] std::string normalize_expires(std::string text) {
    const std::string marker = "\"expires\":";
    std::size_t position = 0;
    while ((position = text.find(marker, position)) != std::string::npos) {
        auto start = position + marker.size();
        while (start < text.size() &&
               (text[start] == ' ' || text[start] == '\t')) {
            ++start;
        }
        auto end = start;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        text.replace(start, end - start, "0");
        position = start + 1;
    }
    return text;
}

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class FakeOAuthHttpClient final : public ai::auth::OAuthHttpClient {
public:
    struct ScriptedResponse {
        int status{200};
        std::string body;
    };

    boost::asio::awaitable<util::Expected<ai::auth::OAuthHttpResponse>> post(
        std::string url,
        std::map<std::string, std::string, std::less<>>,
        std::string body,
        std::stop_token stop_token) override {
        requests.push_back({url, std::move(body)});
        if (stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "fake client cancelled"));
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

    struct Request {
        std::string url;
        std::string body;
    };
    std::map<std::string, std::deque<ScriptedResponse>, std::less<>> responses;
    std::vector<Request> requests;
};

class FakeAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> environment(
        std::string) const override {
        return cch::support::AsyncResult<std::optional<std::string>>(
            std::expected<std::optional<std::string>, cch::support::Error>{
                std::optional<std::string>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<bool> file_exists(
        std::string) const override {
        return cch::support::AsyncResult<bool>(
            std::expected<bool, cch::support::Error>{false});
    }
};

const char kDeviceAuthorizationUrl[] =
    "https://auth.kimi.com/api/oauth/device_authorization";
const char kTokenUrl[] = "https://auth.kimi.com/api/oauth/token";

std::string device_authorization_json() {
    return R"({"user_code":"ABCD-1234","device_code":"device-code-123",)"
           R"("verification_uri":"https://www.kimi.com/code",)"
           R"("verification_uri_complete":"https://www.kimi.com/code?user_code=ABCD-1234",)"
           R"("interval":1,"expires_in":600})";
}

struct LoginHarness {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path auth_path;
    std::shared_ptr<cch::coding_agent::AuthStorage> storage;
    std::shared_ptr<FakeOAuthHttpClient> http{std::make_shared<FakeOAuthHttpClient>()};
    std::shared_ptr<FakeAuthContext> auth_context{std::make_shared<FakeAuthContext>()};
    std::unique_ptr<ai::Models> models;

    explicit LoginHarness() {
        auth_path = workspace.path() / "auth.json";
        workspace.write("auth.json", "{}");
        storage = std::make_shared<cch::coding_agent::AuthStorage>(auth_path);
        models = std::make_unique<ai::Models>(storage, auth_context);
        ai::ProviderAuth provider_auth;
        provider_auth.oauth = ai::auth::make_kimi_coding_oauth_auth(http);
        auto provider = ai::providers::make_kimi_coding_provider(
            std::move(provider_auth), nullptr);
        REQUIRE(models->set_provider(std::move(provider)));
    }

    [[nodiscard]] ai::AuthInteraction interaction(std::stop_token token = {}) const {
        ai::AuthInteraction value;
        value.stop_token = token;
        value.notify = [](const ai::AuthEvent&) {};
        return value;
    }

    /// Seed a stored OAuth credential directly (bypassing login), so
    /// request-time refresh/expiry paths can be driven without a login flow.
    void seed_stored_credential(
        std::string access,
        std::string refresh,
        std::int64_t expires) {
        auto modified = run_async_result(storage->modify(
            "kimi-coding",
            [access = std::move(access), refresh = std::move(refresh), expires](
                std::optional<ai::Credential>)
                -> cch::support::AsyncResult<std::optional<ai::Credential>> {
                return cch::support::AsyncResult<std::optional<ai::Credential>>(
                    std::expected<std::optional<ai::Credential>, cch::support::Error>{
                        std::optional<ai::Credential>{ai::Credential{
                            ai::OAuthCredential{
                                .refresh = refresh,
                                .access = access,
                                .expires = expires,
                            }}}});
            }));
        REQUIRE(modified);
    }

    /// Mark the stored credential as expiring within 5 minutes so the next
    /// request-time resolution refreshes under the store lock.
    void expire_stored_credential() {
        auto modified = run_async_result(storage->modify(
            "kimi-coding",
            [](std::optional<ai::Credential> current)
                -> cch::support::AsyncResult<std::optional<ai::Credential>> {
                REQUIRE(current.has_value());
                auto oauth = std::get<ai::OAuthCredential>(std::move(*current));
                oauth.expires = now_ms() + 60'000;
                return cch::support::AsyncResult<std::optional<ai::Credential>>(
                    std::expected<std::optional<ai::Credential>, cch::support::Error>{
                        std::optional<ai::Credential>{
                            ai::Credential{std::move(oauth)}}});
            }));
        REQUIRE(modified);
    }
};

} // namespace

TEST_CASE("Kimi OAuth lifecycle persists login, refresh rotation, then logout", "[coding_agent][auth][issue344]") {
    LoginHarness harness;
    harness.http->responses[kDeviceAuthorizationUrl] = {
        {200, device_authorization_json()},
    };
    harness.http->responses[kTokenUrl] = {
        {400, R"({"error":"authorization_pending"})"},
        {200,
         R"({"access_token":"dummy-access-token","refresh_token":"dummy-refresh-token","expires_in":3600})"},
        {200,
         R"({"access_token":"dummy-rotated-access-token","refresh_token":"dummy-rotated-refresh-token","expires_in":3600})"},
    };

    // Login persists the oauth record through CredentialStore::modify.
    auto credential = run_async_result(harness.models->login(
        "kimi-coding", ai::AuthType::OAuth, harness.interaction()));
    REQUIRE(credential);
    CHECK(normalize_expires(read_text(harness.auth_path)) ==
          read_fixture("kimi-oauth-after-login.json"));

    // A stored credential that is about to expire refreshes under the store
    // lock and rotates the persisted record before release.
    harness.expire_stored_credential();
    auto resolved = run_async_result(harness.models->get_auth("kimi-coding"));
    REQUIRE(resolved);
    REQUIRE(*resolved);
    CHECK((**resolved).auth.headers.at("Authorization") ==
          "Bearer dummy-rotated-access-token");
    CHECK((**resolved).source == "OAuth");
    CHECK(normalize_expires(read_text(harness.auth_path)) ==
          read_fixture("kimi-oauth-after-refresh.json"));

    // The logout list is CredentialStore::list() metadata ({provider, type}).
    auto before_logout = run_async_result(harness.storage->list());
    REQUIRE(before_logout);
    const std::vector<ai::CredentialInfo> expected_metadata{
        {.provider_id = "kimi-coding", .type = "oauth"},
    };
    CHECK(*before_logout == expected_metadata);

    // Logout removes the stored record locally only; ambient/environment and
    // config-based auth is never stored, so it is untouched by this removal.
    auto removed = run_async_result(harness.models->logout("kimi-coding"));
    REQUIRE(removed);
    auto listed = run_async_result(harness.storage->list());
    REQUIRE(listed);
    CHECK(listed->empty());
    CHECK(read_text(harness.auth_path) == "{}");
}

TEST_CASE("Kimi login cancellation persists nothing", "[coding_agent][auth][issue344]") {
    LoginHarness harness;
    std::stop_source cancel;
    cancel.request_stop();

    auto credential = run_async_result(harness.models->login(
        "kimi-coding",
        ai::AuthType::OAuth,
        harness.interaction(cancel.get_token())));

    REQUIRE(!credential);
    CHECK(credential.error().code == util::ErrorCode::Cancelled);
    CHECK(credential.error().message == "Login cancelled");
    CHECK(read_text(harness.auth_path) == "{}");
}

TEST_CASE("Kimi dead credentials stay in auth.json", "[coding_agent][auth][issue344]") {
    LoginHarness harness;
    harness.seed_stored_credential(
        "dummy-access-token", "dummy-refresh-token", now_ms() + 60'000);
    harness.http->responses[kTokenUrl] = {
        {400, R"({"error":"invalid_grant","error_description":"dead token"})"},
    };

    auto resolved = run_async_result(harness.models->get_auth("kimi-coding"));

    REQUIRE(!resolved);
    CHECK(resolved.error().code == util::ErrorCode::OAuth);
    // The stored credential is preserved for retry: no proactive removal.
    auto persisted = read_text(harness.auth_path);
    CHECK(persisted.find("dummy-refresh-token") != std::string::npos);
    CHECK(persisted.find("dummy-access-token") != std::string::npos);
    auto stored = run_async_result(harness.storage->read("kimi-coding"));
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    const auto& oauth = std::get<ai::OAuthCredential>(**stored);
    CHECK(oauth.access == "dummy-access-token");
    CHECK(oauth.refresh == "dummy-refresh-token");

    // A second request fails the same way (the dead credential is not removed).
    auto again = run_async_result(harness.models->get_auth("kimi-coding"));
    REQUIRE(!again);
    CHECK(again.error().code == util::ErrorCode::OAuth);
    CHECK(read_text(harness.auth_path).find("dummy-refresh-token") !=
          std::string::npos);
}

TEST_CASE("Kimi getAuth applies the OAuth Bearer header before streaming", "[coding_agent][auth][issue344]") {
    LoginHarness harness;
    harness.http->responses[kDeviceAuthorizationUrl] = {
        {200, device_authorization_json()},
    };
    harness.http->responses[kTokenUrl] = {
        {200,
         R"({"access_token":"dummy-access-token","refresh_token":"dummy-refresh-token","expires_in":3600})"},
    };

    auto credential = run_async_result(harness.models->login(
        "kimi-coding", ai::AuthType::OAuth, harness.interaction()));
    REQUIRE(credential);

    auto checked = run_async_result(harness.models->check_auth("kimi-coding"));
    REQUIRE(checked);
    REQUIRE(checked->has_value());
    CHECK((**checked).type == ai::AuthType::OAuth);
    CHECK((**checked).source == "OAuth");
    // checkAuth is side-effect-free: the fresh 1h credential is not refreshed.
    CHECK(harness.http->requests.size() == 2);
}
