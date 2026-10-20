// Application OAuth credential persistence: the cch_mcp flow owns sign-in and
// refresh; this adapter stores the resulting token state in `mcp-auth.json`
// through McpAuthStore (#1065 keeps it in the application; #1066 may narrow
// the seam).

#include "coding_agent/mcp/McpOAuthProvider.hpp"

#include "support/AsyncResultBridge.hpp"

#include <utility>

namespace cch::coding_agent::mcp {

support::AsyncResult<void> login_mcp_server(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthProvider& provider,
        cch::mcp::AuthInteraction interaction) {
    return support::detail::make_async_result(
            [store = std::move(store),
                    server_name = std::move(server_name),
                    server_url = std::move(server_url),
                    &provider,
                    interaction = std::move(interaction)]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                auto credential =
                    co_await support::detail::await_async_result(provider.login(std::move(interaction)));
                if (!credential) {
                    co_return std::unexpected(std::move(credential.error()));
                }
                auto state = mcp_oauth_state_from_credential(*credential, server_url);
                auto stored = store->save(server_name, server_url, state);
                if (!stored) {
                    co_return std::unexpected(std::move(stored.error()));
                }
                co_return support::ExpectedVoid{};
            });
}

support::AsyncResult<void> logout_mcp_server(
        std::shared_ptr<McpAuthStore> store, std::string server_name, std::string server_url) {
    return support::detail::make_async_result(
            [store = std::move(store),
                    server_name = std::move(server_name),
                    server_url = std::move(server_url)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto removed = store->remove(server_name, server_url);
                if (!removed) {
                    co_return std::unexpected(std::move(removed.error()));
                }
                co_return support::ExpectedVoid{};
            });
}

} // namespace cch::coding_agent::mcp
