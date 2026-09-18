#pragma once

#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Auth.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace cch::ai::auth {

/// Token triple parsed from an OAuth token endpoint response: `expires` is a
/// wall-clock millisecond timestamp.
struct OAuthToken {
    std::string access{};
    std::string refresh{};
    std::int64_t expires{0};
};

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
