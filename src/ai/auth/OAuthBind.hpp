#pragma once

#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/ai/OAuthShared.hpp>
#include <cch/support/AsyncResult.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cch::ai::auth {

/// True when the implementation's `login` accepts the optional app context
/// (pi `login(interaction, options?)`). Implementations that predate the
/// `LoginOptions` surface keep their one-argument `login` bound here.
template <typename Impl>
concept OAuthLoginAcceptsOptions = requires(Impl& impl, ai::AuthInteraction interaction) {
    { impl.login(std::move(interaction), std::optional<ai::LoginOptions>{}) };
};

/// Binds one OAuth implementation's login/refresh/to_auth coroutines to the
/// `OAuthAuth` hook surface, each operation crossing the AsyncResult bridge.
template <typename Impl> [[nodiscard]] ai::OAuthAuth bind_oauth_auth(std::string name, std::shared_ptr<Impl> impl) {
    ai::OAuthAuth auth;
    auth.name = std::move(name);
    if constexpr (OAuthLoginAcceptsOptions<Impl>) {
        auth.login =
                [impl](ai::AuthInteraction interaction,
                        std::optional<ai::LoginOptions> options) -> cch::support::AsyncResult<ai::OAuthCredential> {
            return cch::support::detail::make_async_result(
                    [impl, interaction = std::move(interaction), options = std::move(options)]() mutable
                            -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                        co_return co_await impl->login(std::move(interaction), std::move(options));
                    });
        };
    } else {
        auth.login = [impl](ai::AuthInteraction interaction,
                             std::optional<ai::LoginOptions>) -> cch::support::AsyncResult<ai::OAuthCredential> {
            return cch::support::detail::make_async_result(
                    [impl, interaction = std::move(interaction)]() mutable
                            -> boost::asio::awaitable<support::Expected<ai::OAuthCredential>> {
                        co_return co_await impl->login(std::move(interaction));
                    });
        };
    }
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
