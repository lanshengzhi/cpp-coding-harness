// MCP OAuth request-time token resolution (spec #882, ticket #884; the store
// replaced #875's `auth.json` reuse). The state lives in `mcp-auth.json`; this
// resolver loads it and, when it is within pi's 30-second refresh skew, joins
// the server's one shared refresh and persists the rotated state before the
// request goes out (ticket #1189). A caller that is cancelled — or whose
// request times out — stops observing the refresh but does not abort it, so a
// rotated refresh token is never lost. There is no second credential store and
// no unauthenticated fallback.

#include "coding_agent/mcp/McpOAuthTokenResolver.hpp"

#include "coding_agent/mcp/McpOAuthRefresh.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Timestamps.hpp>
#include <cch/mcp/Auth.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

/// pi `REFRESH_SKEW_MS`: an access token this close to expiry is refreshed
/// before it is sent (ticket #1189 replaced the five-minute Models margin).
inline constexpr auto kRefreshSkew = std::chrono::seconds{30};

[[nodiscard]] bool token_expires_soon(const cch::mcp::OAuthCredential& credential) {
    return credential.expires <= ai::current_timestamp_ms() + kRefreshSkew.count() * 1000;
}

/// The explicit re-login error: the shared `/login` guidance (pi's
/// `formatOauthReauthenticateMessage`) so the user learns no second credential
/// story. The server name is carried in the detail.
[[nodiscard]] support::Error re_login_error(
        const std::string& server_name, const std::string& provider_id, std::string detail) {
    return support::make_error(support::ErrorCode::OAuth,
            cch::mcp::format_oauth_reauthenticate_message(provider_id),
            "MCP server '" + server_name + "': " + std::move(detail));
}

[[nodiscard]] std::map<std::string, std::string> bearer_headers(std::string_view access) {
    return std::map<std::string, std::string>{{"Authorization", "Bearer " + std::string{access}}};
}

/// The one shared, uncancellable refresh body: acquire the cross-process lock,
/// re-check the stored access token, refresh, persist, and return the
/// credential. A waiter observes it only while `stop_token` is not requested;
/// the operation itself owns the store and provider and keeps running.
[[nodiscard]] support::AsyncResult<cch::mcp::OAuthCredential> coordinated_refresh(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        std::shared_ptr<cch::mcp::OAuthProvider> provider,
        std::shared_ptr<McpOAuthRefresh> refresh,
        std::string stale_access,
        std::stop_token stop_token) {
    return support::detail::make_async_result(
            [store,
                    server_name = std::move(server_name),
                    server_url = std::move(server_url),
                    provider = std::move(provider),
                    refresh = std::move(refresh),
                    stale_access = std::move(stale_access),
                    stop_token]() -> boost::asio::awaitable<support::Expected<cch::mcp::OAuthCredential>> {
                const std::string provider_id = cch::mcp::mcp_oauth_provider_id(server_name);
                auto refreshed = co_await support::detail::await_async_result(refresh->refresh(
                        [store, server_name, server_url, provider, provider_id, stale_access]()
                                -> boost::asio::awaitable<support::Expected<cch::mcp::OAuthCredential>> {
                            // The cross-process lock is held from reading the
                            // tokens to saving the rotated ones, so two
                            // processes never refresh with the same grant.
                            auto lock = co_await support::detail::await_async_result(
                                    store->refresh_lock(server_name, server_url));
                            if (!lock) {
                                co_return std::unexpected(std::move(lock.error()));
                            }
                            auto locked_state = store->load(server_name, server_url);
                            if (!locked_state) {
                                co_return std::unexpected(std::move(locked_state.error()));
                            }
                            std::optional<cch::mcp::OAuthCredential> current;
                            if (locked_state->has_value()) {
                                current = cch::mcp::mcp_oauth_credential_from_state(**locked_state);
                            }
                            if (!current.has_value()) {
                                co_return std::unexpected(
                                        re_login_error(server_name, provider_id, "no stored OAuth credential"));
                            }
                            // Another process (or a sign-in) already rotated
                            // the grant while we waited for the lock: reuse it
                            // instead of refreshing again.
                            if (current->access != stale_access) {
                                co_return *current;
                            }
                            if (current->refresh.empty()) {
                                co_return std::unexpected(
                                        re_login_error(server_name, provider_id, "credential has no refresh token"));
                            }
                            auto rotated =
                                    co_await support::detail::await_async_result(provider->refresh(*current, {}));
                            if (!rotated) {
                                co_return std::unexpected(std::move(rotated.error()));
                            }
                            auto next = cch::mcp::mcp_oauth_state_from_credential(*rotated, server_url);
                            if (auto saved = store->save(server_name, server_url, next); !saved) {
                                co_return std::unexpected(std::move(saved.error()));
                            }
                            co_return std::move(*rotated);
                        },
                        stop_token));
                if (!refreshed) {
                    if (refreshed.error().code == support::ErrorCode::OAuth) {
                        std::string detail =
                                refreshed.error().detail.empty() ? refreshed.error().message : refreshed.error().detail;
                        co_return std::unexpected(re_login_error(server_name, provider_id, std::move(detail)));
                    }
                    co_return std::unexpected(std::move(refreshed.error()));
                }
                co_return std::move(**refreshed);
            });
}

} // namespace

struct McpOAuthTokenResolver::State {
    std::shared_ptr<McpOAuthRefresh> refresh{std::make_shared<McpOAuthRefresh>()};
    /// pi `StreamableHttpTransport.lastToken`: the access token the last
    /// request actually used. Close's DELETE reuses it instead of asking the
    /// auth source for a (possibly refreshed) token.
    std::string last_access_token;
    bool has_last_access_token{false};
};

McpOAuthTokenResolver::McpOAuthTokenResolver(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        std::shared_ptr<cch::mcp::OAuthProvider> provider)
    : store_(std::move(store)), server_name_(std::move(server_name)), server_url_(std::move(server_url)),
      provider_(std::move(provider)), state_(std::make_shared<State>()) {}

McpOAuthTokenResolver::McpOAuthTokenResolver(McpOAuthTokenResolver&&) noexcept = default;
McpOAuthTokenResolver& McpOAuthTokenResolver::operator=(McpOAuthTokenResolver&&) noexcept = default;
McpOAuthTokenResolver::~McpOAuthTokenResolver() = default;

support::AsyncResult<void> McpOAuthTokenResolver::settled() { return state_->refresh->settled(); }

support::AsyncResult<std::map<std::string, std::string>> McpOAuthTokenResolver::current_headers(
        std::stop_token stop_token, cch::mcp::RequestAuthPurpose purpose) {
    return support::detail::make_async_result(
            [store = store_,
                    server_name = server_name_,
                    server_url = server_url_,
                    provider = provider_,
                    state = state_,
                    stop_token,
                    purpose]() -> boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> {
                const std::string provider_id = cch::mcp::mcp_oauth_provider_id(server_name);
                auto store_state = store->load(server_name, server_url);
                if (!store_state) {
                    co_return std::unexpected(std::move(store_state.error()));
                }
                std::optional<cch::mcp::OAuthCredential> credential;
                if (store_state->has_value()) {
                    credential = cch::mcp::mcp_oauth_credential_from_state(**store_state);
                }
                if (purpose == cch::mcp::RequestAuthPurpose::SessionClose) {
                    // pi `StreamableHttpTransport.close`: reuse the last token
                    // the connection sent; never ask for a refresh here.
                    if (state->has_last_access_token) {
                        co_return bearer_headers(state->last_access_token);
                    }
                    if (credential.has_value()) {
                        co_return bearer_headers(credential->access);
                    }
                    co_return std::map<std::string, std::string>{};
                }
                if (!credential.has_value()) {
                    co_return std::unexpected(re_login_error(server_name, provider_id, "no stored OAuth credential"));
                }
                if (stop_token.stop_requested()) {
                    co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Login cancelled"));
                }
                if (token_expires_soon(*credential)) {
                    auto refreshed = co_await support::detail::await_async_result(coordinated_refresh(
                            store, server_name, server_url, provider, state->refresh, credential->access, stop_token));
                    if (!refreshed) {
                        co_return std::unexpected(std::move(refreshed.error()));
                    }
                    credential = std::move(*refreshed);
                }
                if (token_expires_soon(*credential)) {
                    // The refresh left a credential with no usable lifetime:
                    // re-login is required and the resolver does not loop.
                    co_return std::unexpected(re_login_error(server_name, provider_id, "credential is expired"));
                }
                state->last_access_token = credential->access;
                state->has_last_access_token = true;
                co_return bearer_headers(credential->access);
            });
}

support::AsyncResult<bool> McpOAuthTokenResolver::recover_unauthorized(
        std::stop_token stop_token, std::string rejected_authorization) {
    return support::detail::make_async_result([store = store_,
                                                      server_name = server_name_,
                                                      server_url = server_url_,
                                                      provider = provider_,
                                                      state = state_,
                                                      rejected = std::move(rejected_authorization),
                                                      stop_token]() -> boost::asio::awaitable<support::Expected<bool>> {
        auto store_state = store->load(server_name, server_url);
        if (!store_state) {
            co_return std::unexpected(std::move(store_state.error()));
        }
        if (!store_state->has_value()) {
            co_return false;
        }
        auto credential = cch::mcp::mcp_oauth_credential_from_state(**store_state);
        if (!credential.has_value() || credential->refresh.empty()) {
            // Nothing to rotate: the 401 stays the explicit re-login
            // error, and the rejected credential is not sent twice.
            co_return false;
        }
        std::string stale_access = std::move(rejected);
        if (stale_access.starts_with("Bearer ")) {
            stale_access.erase(0, std::string_view{"Bearer "}.size());
        }
        auto refreshed = co_await support::detail::await_async_result(coordinated_refresh(
                store, server_name, server_url, provider, state->refresh, stale_access, stop_token));
        if (!refreshed) {
            if (refreshed.error().code == support::ErrorCode::Cancelled) {
                // The caller settled; the shared refresh keeps going.
                co_return false;
            }
            co_return std::unexpected(std::move(refreshed.error()));
        }
        state->last_access_token = refreshed->access;
        state->has_last_access_token = true;
        co_return true;
    });
}

} // namespace cch::coding_agent::mcp
