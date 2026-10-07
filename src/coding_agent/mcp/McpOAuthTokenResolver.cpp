// MCP OAuth request-time token resolution (spec #882, ticket #884; the store
// replaced #875's `auth.json` reuse). The state lives in `mcp-auth.json`; this
// resolver loads it, and when it is within the five-minute validity margin
// refreshes it through the provider and persists the rotated state before the
// request goes out. There is no second credential store and no unauthenticated
// fallback.

#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"

#include <cch/ai/Timestamps.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <map>
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

McpOAuthTokenResolver::McpOAuthTokenResolver(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        std::shared_ptr<McpOAuthProvider> provider)
    : store_(std::move(store)), server_name_(std::move(server_name)), server_url_(std::move(server_url)),
      provider_(std::move(provider)) {}

McpOAuthTokenResolver::McpOAuthTokenResolver(McpOAuthTokenResolver&&) noexcept = default;
McpOAuthTokenResolver& McpOAuthTokenResolver::operator=(McpOAuthTokenResolver&&) noexcept = default;
McpOAuthTokenResolver::~McpOAuthTokenResolver() = default;

support::AsyncResult<std::map<std::string, std::string>> McpOAuthTokenResolver::current_headers() {
    return support::detail::make_async_result(
            [store = store_, server_name = server_name_, server_url = server_url_, provider = provider_]()
                    -> boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> {
                const std::string provider_id = mcp_oauth_provider_id(server_name);
                auto state = store->load(server_name, server_url);
                if (!state) {
                    co_return std::unexpected(std::move(state.error()));
                }
                std::optional<ai::OAuthCredential> credential;
                if (state->has_value()) {
                    credential = mcp_oauth_credential_from_state(**state);
                }
                if (!credential.has_value()) {
                    co_return std::unexpected(re_login_error(server_name, provider_id, "no stored OAuth credential"));
                }
                if (token_expires_soon(*credential)) {
                    // One refresh attempt: a failed refresh (including an
                    // `invalid_grant`) propagates, is never retried, and leaves
                    // the stored state untouched.
                    auto refreshed = co_await provider->refresh(*credential);
                    if (!refreshed) {
                        if (refreshed.error().code == support::ErrorCode::OAuth) {
                            std::string detail = refreshed.error().detail.empty() ? refreshed.error().message
                                                                                  : refreshed.error().detail;
                            co_return std::unexpected(re_login_error(server_name, provider_id, std::move(detail)));
                        }
                        co_return std::unexpected(std::move(refreshed.error()));
                    }
                    auto rotated = mcp_oauth_state_from_credential(*refreshed, server_url);
                    if (auto saved = store->save(server_name, server_url, rotated); !saved) {
                        co_return std::unexpected(std::move(saved.error()));
                    }
                    credential = std::move(*refreshed);
                }
                if (token_expires_soon(*credential)) {
                    // The refresh left a credential with no usable lifetime:
                    // re-login is required and the resolver does not loop.
                    co_return std::unexpected(re_login_error(server_name, provider_id, "credential is expired"));
                }
                CCH_TRY(auth, co_await provider->to_auth(*credential));
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
