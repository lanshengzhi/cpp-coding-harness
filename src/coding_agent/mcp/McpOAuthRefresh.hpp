#pragma once

// pi's shared MCP OAuth refresh (`packages/coding-agent/src/extensions/mcp/
// oauth.ts` `createMcpAuthProvider`'s `refreshing` promise, ticket #1189).
// One refresh runs per server/URL, every request waits for it only while its
// own cancellation is not requested, and the refresh itself is never cancelled
// by a waiter: a token the authorization server already answered may have
// rotated the refresh token, so its answer must be saved. `settled()` is pi's
// provider `settled()`, the seam shutdown uses to keep a rotated grant.

#include <cch/mcp/Auth.hpp>
#include <cch/support/AsyncResult.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <stop_token>

namespace cch::coding_agent::mcp {

class McpOAuthRefresh final : public std::enable_shared_from_this<McpOAuthRefresh> {
public:
    /// The one refresh body: acquire the cross-process lock, re-check the
    /// stored access token, refresh, persist, and return the credential.
    using Operation = std::move_only_function<boost::asio::awaitable<support::Expected<cch::mcp::OAuthCredential>>()>;

    McpOAuthRefresh();
    McpOAuthRefresh(const McpOAuthRefresh&) = delete;
    McpOAuthRefresh& operator=(const McpOAuthRefresh&) = delete;
    ~McpOAuthRefresh();

    /// Start the one in-flight refresh, or join it. A waiter whose
    /// `stop_token` fires completes with a cancellation error immediately;
    /// the refresh keeps running. A failed refresh is reported to every
    /// waiter and is not retried here.
    [[nodiscard]] support::AsyncResult<std::optional<cch::mcp::OAuthCredential>> refresh(
            Operation operation, std::stop_token stop_token = {});

    /// Resolve once no refresh is in flight (pi `settled()`).
    [[nodiscard]] support::AsyncResult<void> settled();

    /// Whether a refresh is currently running (test and diagnostic seam over
    /// the same state `settled()` waits on).
    [[nodiscard]] bool in_flight() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::mcp
