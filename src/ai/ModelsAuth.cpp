#include <cch/ai/Models.hpp>

#include "support/AsyncResultBridge.hpp"
#include "ai/Headers.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "ai/Timestamps.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/BoostBeastWebSocketTransport.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "ai/providers/ProviderTestAccess.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "SimpleOptions.hpp"
#include "support/BoundedText.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#include <exception>
#endif
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ModelsImpl.hpp"

namespace cch::ai {
namespace {

[[nodiscard]] std::string_view auth_type_string(AuthType type) {
    switch (type) {
    case AuthType::ApiKey:
        return "api_key";
    case AuthType::OAuth:
        return "oauth";
    }
    return "oauth";
}

} // namespace

cch::support::AsyncResult<std::optional<AuthCheck>> Models::check_auth(std::string provider_id) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthCheck>>> {
                const auto selected = impl_->provider(provider_id);
                if (!selected) {
                    co_return std::optional<AuthCheck>{};
                }

                auto stored = co_await invoke_async_operation([&]() { return impl_->credentials->read(provider_id); });
                if (!stored) {
                    co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                            "Credential store read failed for " + provider_id,
                            stored.error()));
                }

                auto& auth = selected->auth();
                if (*stored) {
                    if (std::holds_alternative<OAuthCredential>(**stored)) {
                        if (!auth.oauth) {
                            co_return std::optional<AuthCheck>{};
                        }
                        co_return AuthCheck{.source = "OAuth", .type = AuthType::OAuth};
                    }
                    if (!auth.api_key) {
                        co_return std::optional<AuthCheck>{};
                    }
                    const auto credential = std::get<ApiKeyCredential>(**stored);
                    if (auth.api_key->check) {
                        auto checked = co_await invoke_async_operation(
                                [&]() { return auth.api_key->check(*impl_->auth_context, credential); });
                        if (!checked) {
                            co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                                    "API key auth check failed for provider " + provider_id,
                                    checked.error()));
                        }
                        co_return std::move(*checked);
                    }
                    CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, credential));
                    if (!resolved) {
                        co_return std::optional<AuthCheck>{};
                    }
                    co_return AuthCheck{
                            .source = resolved->source,
                            .type = AuthType::ApiKey,
                    };
                }

                if (!auth.api_key) {
                    co_return std::optional<AuthCheck>{};
                }
                if (auth.api_key->check) {
                    auto checked = co_await invoke_async_operation(
                            [&]() { return auth.api_key->check(*impl_->auth_context, std::nullopt); });
                    if (!checked) {
                        co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                                "API key auth check failed for provider " + provider_id,
                                checked.error()));
                    }
                    co_return std::move(*checked);
                }
                CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, std::nullopt));
                if (!resolved) {
                    co_return std::optional<AuthCheck>{};
                }
                co_return AuthCheck{
                        .source = resolved->source,
                        .type = AuthType::ApiKey,
                };
            });
}

cch::support::AsyncResult<std::optional<AuthResult>> Models::get_auth(
        std::string provider_id, std::optional<std::string> explicit_api_key) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id), explicit_api_key = std::move(explicit_api_key)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                const auto selected = impl_->provider(provider_id);
                if (!selected) {
                    co_return std::optional<AuthResult>{};
                }
                auto& auth = selected->auth();

                if (explicit_api_key && auth.api_key) {
                    ApiKeyCredential credential;
                    credential.key = std::move(explicit_api_key);
                    CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, std::move(credential)));
                    co_return resolved;
                }

                auto stored = co_await invoke_async_operation([&]() { return impl_->credentials->read(provider_id); });
                if (!stored) {
                    co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                            "Credential store read failed for " + provider_id,
                            stored.error()));
                }
                if (*stored) {
                    if (auto* oauth = std::get_if<OAuthCredential>(&**stored)) {
                        if (!auth.oauth) {
                            co_return std::optional<AuthResult>{};
                        }
                        CCH_TRY(resolved, co_await impl_->resolve_oauth(*selected, *auth.oauth, *oauth));
                        co_return resolved;
                    }
                    if (!auth.api_key) {
                        co_return std::optional<AuthResult>{};
                    }
                    CCH_TRY(resolved,
                            co_await impl_->resolve_api_key(
                                    *selected, *auth.api_key, std::get<ApiKeyCredential>(**stored)));
                    co_return resolved;
                }

                if (!auth.api_key) {
                    co_return std::optional<AuthResult>{};
                }
                CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, std::nullopt));
                co_return resolved;
            });
}

cch::support::AsyncResult<std::optional<AuthResult>> Models::get_auth(
        Model model_value, std::optional<std::string> explicit_api_key) {
    return support::detail::make_async_result(
            [this, model_value = std::move(model_value), explicit_api_key = std::move(explicit_api_key)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                CCH_TRY(resolved,
                        co_await support::detail::await_async_result(
                                get_auth(model_value.provider, std::move(explicit_api_key))));
                if (!resolved || !model_value.headers) {
                    co_return resolved;
                }
                merge_headers(resolved->auth.headers, *model_value.headers);
                co_return resolved;
            });
}

cch::support::AsyncResult<void> Models::logout(std::string provider_id) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto removed =
                        co_await invoke_async_operation([&]() { return impl_->credentials->remove(provider_id); });
                if (!removed) {
                    co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                            "Credential store delete failed for " + provider_id,
                            removed.error()));
                }
                co_return support::ExpectedVoid{};
            });
}

cch::support::AsyncResult<Credential> Models::login(
        std::string provider_id, AuthType type, AuthInteraction interaction) {
    return support::detail::make_async_result([this,
                                                      provider_id = std::move(provider_id),
                                                      type,
                                                      interaction = std::move(interaction)]() mutable
                                                      -> boost::asio::awaitable<support::Expected<Credential>> {
        const auto selected = impl_->provider(provider_id);
        if (!selected) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Provider, "Unknown provider: " + provider_id));
        }
        auto& auth = selected->auth();
        const auto type_name = std::string{auth_type_string(type)};

        support::Expected<Credential> flow_result;
        if (type == AuthType::OAuth) {
            if (!auth.oauth || !auth.oauth->login) {
                co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
                        std::string{selected->name()} + " does not support " + type_name + " login"));
            }
            auto credential =
                    co_await invoke_async_operation([&]() { return auth.oauth->login(std::move(interaction)); });
            if (!credential) {
                // Login-flow failures propagate unwrapped to the host.
                co_return std::unexpected(std::move(credential.error()));
            }
            flow_result = std::move(*credential);
        } else {
            if (!auth.api_key || !auth.api_key->login) {
                co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
                        std::string{selected->name()} + " does not support " + type_name + " login"));
            }
            auto credential =
                    co_await invoke_async_operation([&]() { return auth.api_key->login(std::move(interaction)); });
            if (!credential) {
                co_return std::unexpected(std::move(credential.error()));
            }
            flow_result = std::move(*credential);
        }

        // Persist exclusively through CredentialStore::modify, the only write
        // path. Only CredentialStore failures wrap as the `auth` category.
        support::Expected<Credential> login_credential = std::move(flow_result);
        auto stored = co_await invoke_async_operation([&]() -> cch::support::AsyncResult<std::optional<Credential>> {
            return impl_->credentials->modify(provider_id,
                    [credential = login_credential](
                            std::optional<Credential>) -> cch::support::AsyncResult<std::optional<Credential>> {
                        return cch::support::AsyncResult<std::optional<Credential>>(
                                std::expected<std::optional<Credential>, cch::support::Error>{
                                        std::optional<Credential>{*credential}});
                    });
        });
        if (!stored) {
            co_return std::unexpected(categorized_error(
                    support::ErrorCode::Auth, "Credential store modify failed for " + provider_id, stored.error()));
        }
        co_return std::move(login_credential);
    });
}

} // namespace cch::ai
