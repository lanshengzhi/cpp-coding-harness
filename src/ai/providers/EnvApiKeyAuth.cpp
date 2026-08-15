#include "EnvApiKeyAuth.hpp"

#include <cch/support/Error.hpp>

#include "ai/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <utility>

namespace cch::ai::providers {
namespace {

/// Borrowed context and environment_names must outlive the returned awaitable.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> resolve_api_key(
    const AuthContext& context,
    std::optional<ApiKeyCredential> credential,
    const std::vector<std::string>& environment_names) {
    if (credential && credential->key && !credential->key->empty()) {
        ProviderEnv env;
        env.insert(credential->env.begin(), credential->env.end());
        co_return AuthResult{
            .auth = ModelAuth{.api_key = *credential->key},
            .env = std::move(env),
            .source = "stored credential",
        };
    }
    for (const auto& name : environment_names) {
        CCH_TRY(value, co_await detail::await_async_result(context.environment(name)));
        if (value && !value->empty()) {
            co_return AuthResult{
                .auth = ModelAuth{.api_key = *value},
                .env = {},
                .source = name,
            };
        }
    }
    co_return std::optional<AuthResult>{};
}

} // namespace

[[nodiscard]] ProviderAuth make_env_api_key_auth(
    std::string provider_name,
    std::vector<std::string> environment_names) {
    ApiKeyAuth api_key;
    api_key.name = std::move(provider_name);
    api_key.check = [environment_names](
                        const AuthContext& context,
                        std::optional<ApiKeyCredential> credential)
        -> cch::support::AsyncResult<std::optional<AuthCheck>> {
        return detail::make_async_result(
            [&context, credential = std::move(credential), environment_names]()
                -> boost::asio::awaitable<support::Expected<std::optional<AuthCheck>>> {
                CCH_TRY(resolved, co_await resolve_api_key(
                    context, std::move(credential), environment_names));
                if (!resolved) {
                    co_return std::optional<AuthCheck>{};
                }
                co_return AuthCheck{
                    .source = resolved->source,
                    .type = AuthType::ApiKey,
                };
            });
    };
    api_key.resolve = [environment_names = std::move(environment_names)](
                          const AuthContext& context,
                          std::optional<ApiKeyCredential> credential)
        -> cch::support::AsyncResult<std::optional<AuthResult>> {
        return detail::make_async_result(
            [&context, credential = std::move(credential), environment_names]()
                -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                CCH_TRY(resolved, co_await resolve_api_key(
                    context, std::move(credential), environment_names));
                co_return resolved;
            });
    };
    return ProviderAuth{.api_key = std::move(api_key)};
}

} // namespace cch::ai::providers
