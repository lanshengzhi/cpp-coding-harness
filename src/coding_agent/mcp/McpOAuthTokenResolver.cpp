// MCP OAuth request-time token resolution (spec #865, ticket #875). The
// credential lives in the shared `auth.json` through AuthStorage; this resolver
// reads it and, when it is within the five-minute validity margin, refreshes it
// through the same `CredentialStore::modify` transaction the Models runtime
// uses — the refresh hook runs on AuthStorage's own execution context, so it is
// off the caller's Runtime loop, and the rotated credential is persisted before
// the request goes out. There is no second credential store and no
// unauthenticated fallback.

#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"

#include <cch/ai/Timestamps.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

/// The request-time refresh margin: a credential expiring within five minutes is
/// refreshed before it is used, matching the Models runtime's
/// `kOAuthMinimumValidity` (ADR 0032).
inline constexpr auto kOAuthMinimumValidity = std::chrono::minutes{5};

[[nodiscard]] bool token_expires_soon(const ai::OAuthCredential& credential) {
    return credential.expires <= ai::current_timestamp_ms() + kOAuthMinimumValidity.count() * 1000;
}

/// The explicit re-login error: the shared `/login` guidance (pi's
/// `formatOauthReauthenticateMessage`) so the user learns no second credential
/// story. The server name is carried in the detail.
[[nodiscard]] support::Error re_login_error(
        const std::string& server_name, const std::string& provider_id, std::string detail) {
    return support::make_error(support::ErrorCode::OAuth,
            format_oauth_reauthenticate_message(provider_id),
            "MCP server '" + server_name + "': " + std::move(detail));
}

} // namespace

McpOAuthTokenResolver::McpOAuthTokenResolver(std::shared_ptr<ai::CredentialStore> credentials,
        std::string provider_id,
        std::string server_name,
        std::shared_ptr<McpOAuthProvider> provider)
    : credentials_(std::move(credentials)), provider_id_(std::move(provider_id)), server_name_(std::move(server_name)),
      provider_(std::move(provider)) {}

McpOAuthTokenResolver::McpOAuthTokenResolver(McpOAuthTokenResolver&&) noexcept = default;
McpOAuthTokenResolver& McpOAuthTokenResolver::operator=(McpOAuthTokenResolver&&) noexcept = default;
McpOAuthTokenResolver::~McpOAuthTokenResolver() = default;

support::AsyncResult<std::map<std::string, std::string>> McpOAuthTokenResolver::current_headers() {
    return support::detail::make_async_result(
            [credentials = credentials_, provider_id = provider_id_, server_name = server_name_, provider = provider_]()
                    -> boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> {
                // One `modify` transaction: fresh cross-process read, then a
                // refresh only when the credential is still expiring, persisted
                // before the request continues. A refresh failure propagates
                // unchanged and is mapped below; nothing is retried here, so an
                // `invalid_grant` cannot loop.
                auto modified = credentials->modify(provider_id,
                        [provider](std::optional<ai::Credential> current)
                                -> support::AsyncResult<std::optional<ai::Credential>> {
                            return support::detail::make_async_result(
                                    [provider, current = std::move(current)]() mutable
                                            -> boost::asio::awaitable<
                                                    support::Expected<std::optional<ai::Credential>>> {
                                        const auto* oauth =
                                                current ? std::get_if<ai::OAuthCredential>(&*current) : nullptr;
                                        if (oauth == nullptr || !token_expires_soon(*oauth)) {
                                            // Missing, non-OAuth, or still valid:
                                            // leave the record unchanged; the
                                            // caller decides from the returned
                                            // value.
                                            co_return std::optional<ai::Credential>{};
                                        }
                                        CCH_TRY(refreshed, co_await provider->refresh(*oauth));
                                        co_return std::optional<ai::Credential>{ai::Credential{std::move(refreshed)}};
                                    });
                        });
                auto modified_outcome = co_await support::detail::await_async_result(std::move(modified));
                if (!modified_outcome) {
                    // An `invalid_grant` (dead or already-rotated refresh token)
                    // is mapped to the shared re-login guidance; any other store
                    // or transport failure propagates unchanged. Both are
                    // explicit, and neither is retried.
                    if (modified_outcome.error().code == support::ErrorCode::OAuth) {
                        std::string detail = modified_outcome.error().detail.empty() ? modified_outcome.error().message
                                                                                     : modified_outcome.error().detail;
                        co_return std::unexpected(re_login_error(server_name, provider_id, std::move(detail)));
                    }
                    co_return std::unexpected(std::move(modified_outcome.error()));
                }
                auto outcome = std::move(*modified_outcome);
                const auto* oauth = outcome ? std::get_if<ai::OAuthCredential>(&*outcome) : nullptr;
                if (oauth == nullptr) {
                    co_return std::unexpected(re_login_error(server_name, provider_id, "no stored OAuth credential"));
                }
                if (token_expires_soon(*oauth)) {
                    // The stored credential is still expiring: the refresh was
                    // skipped (for example a concurrent writer holds it) or
                    // returned a credential with no usable lifetime. Re-login is
                    // required; the resolver does not loop.
                    co_return std::unexpected(re_login_error(server_name, provider_id, "credential is expired"));
                }
                CCH_TRY(auth, co_await provider->to_auth(*oauth));
                std::map<std::string, std::string> headers;
                for (const auto& [name, value] : auth.headers) {
                    headers.insert_or_assign(name, value);
                }
                if (auth.api_key && !auth.api_key->empty()) {
                    headers.insert_or_assign("Authorization", "Bearer " + *auth.api_key);
                }
                co_return headers;
            });
}

} // namespace cch::coding_agent::mcp
