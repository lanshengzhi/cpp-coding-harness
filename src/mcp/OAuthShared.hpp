#pragma once

// Private shared OAuth mechanics for cch_mcp's flow/provider implementation.

#include <cch/mcp/Auth.hpp>
#include <cch/mcp/OAuthHttpClient.hpp>

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::mcp {

/// Token triple parsed from an OAuth token endpoint response: `expires` is a
/// wall-clock millisecond timestamp.
struct OAuthToken {
    std::string access{};
    std::string refresh{};
    std::int64_t expires{0};
};

/// Shared pi `fetchWithLoginCancellation` behavior for OAuth token exchanges.
[[nodiscard]] inline boost::asio::awaitable<support::Expected<OAuthHttpResponse>> post_with_login_cancellation(
        std::shared_ptr<OAuthHttpClient> http_client,
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) {
    auto response = co_await support::detail::await_async_result(
            http_client->post(std::move(url), std::move(headers), std::move(body), stop_token));
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
template <typename Event> void notify_best_effort(AuthNotifyHook& notify, Event&& event) {
    notify(AuthEvent{std::forward<Event>(event)});
}

} // namespace cch::mcp
