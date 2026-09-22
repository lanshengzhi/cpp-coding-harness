#pragma once

#include "OAuthHttpClient.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <stop_token>
#include <utility>

namespace cch::ai::auth {

/// Token triple parsed from an OAuth token endpoint response: `expires` is a
/// wall-clock millisecond timestamp.
struct OAuthToken {
    std::string access{};
    std::string refresh{};
    std::int64_t expires{0};
};

/// Shared pi `fetchWithLoginCancellation` behavior for OAuth token exchanges.
/// The HTTP client is owned by value because the coroutine may suspend.
[[nodiscard]] inline boost::asio::awaitable<support::Expected<OAuthHttpResponse>> post_with_login_cancellation(
        std::shared_ptr<OAuthHttpClient> http_client,
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) {
    auto response = co_await http_client->post(std::move(url), std::move(headers), std::move(body), stop_token);
    if (!response) {
        if (stop_token.stop_requested() || response.error().code == support::ErrorCode::Cancelled) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Login cancelled"));
        }
        co_return std::unexpected(std::move(response.error()));
    }
    co_return *response;
}

/// Wall-clock millisecond expiry from an `expires_in` seconds offset.
[[nodiscard]] inline std::int64_t oauth_token_expiry_ms(double expires_in_seconds, std::int64_t now_ms) {
    return now_ms + static_cast<std::int64_t>(expires_in_seconds * 1000.0);
}

/// Best-effort display: notify never vetoes login. The hook is contractually
/// non-throwing (AuthNotifyHook).
template <typename Event> void notify_best_effort(ai::AuthNotifyHook& notify, Event&& event) {
    notify(ai::AuthEvent{std::forward<Event>(event)});
}

/// Binds one OAuth implementation's login/refresh/to_auth coroutines to the
/// `OAuthAuth` hook surface, each operation crossing the AsyncResult bridge.
template <typename Impl> [[nodiscard]] ai::OAuthAuth bind_oauth_auth(std::string name, std::shared_ptr<Impl> impl) {
    ai::OAuthAuth auth;
    auth.name = std::move(name);
    auth.login = [impl](ai::AuthInteraction interaction) -> cch::support::AsyncResult<ai::OAuthCredential> {
        return cch::support::detail::make_async_result(
                [impl, interaction = std::move(interaction)]() mutable
                        -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                    co_return co_await impl->login(std::move(interaction));
                });
    };
    auth.refresh = [impl](ai::OAuthCredential credential) -> cch::support::AsyncResult<ai::OAuthCredential> {
        return cch::support::detail::make_async_result(
                [impl, credential = std::move(credential)]()
                        -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                    co_return co_await impl->refresh(std::move(credential));
                });
    };
    auth.to_auth = [impl](const ai::OAuthCredential& credential) -> cch::support::AsyncResult<ai::ModelAuth> {
        return cch::support::detail::make_async_result(
                [impl, credential]() -> boost::asio::awaitable<support::Expected<ai::ModelAuth>> {
                    co_return co_await impl->to_auth(credential);
                });
    };
    return auth;
}

} // namespace cch::ai::auth
