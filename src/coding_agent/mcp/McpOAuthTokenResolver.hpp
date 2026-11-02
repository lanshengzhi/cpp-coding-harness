#pragma once

#include "coding_agent/mcp/McpAuthStore.hpp"

#include <cch/mcp/OAuthProvider.hpp>
#include <cch/mcp/RequestAuthSource.hpp>

#include <cch/support/AsyncResult.hpp>

#include <map>
#include <memory>
#include <string>

namespace cch::coding_agent::mcp {

class McpOAuthRefresh;

/// `mcp-auth.json`-backed OAuth resolution for one MCP server (spec #882,
/// ticket #884; the store replaced #875's `auth.json` reuse). Each call loads
/// the server's state and, when it is within pi's 30-second refresh skew,
/// joins the server's one shared refresh (ticket #1189), persists the rotated
/// state, and only then returns the headers. A waiter observes the refresh
/// only while its request is live: cancellation or timeout settles the caller
/// immediately while the token exchange keeps running, so a rotated refresh
/// token is never lost. A missing state, a state without tokens, or a failed
/// refresh is an explicit re-login error — never a silent unauthenticated
/// request, and never a retry loop.
class McpOAuthTokenResolver final : public cch::mcp::RequestAuthSource {
public:
    McpOAuthTokenResolver(std::shared_ptr<McpAuthStore> store,
            std::string server_name,
            std::string server_url,
            std::shared_ptr<cch::mcp::OAuthProvider> provider);

    McpOAuthTokenResolver(McpOAuthTokenResolver&&) noexcept;
    McpOAuthTokenResolver& operator=(McpOAuthTokenResolver&&) noexcept;
    ~McpOAuthTokenResolver() override;
    McpOAuthTokenResolver(const McpOAuthTokenResolver&) = delete;
    McpOAuthTokenResolver& operator=(const McpOAuthTokenResolver&) = delete;

    [[nodiscard]] support::AsyncResult<std::map<std::string, std::string>> current_headers(
            std::stop_token stop_token = {},
            cch::mcp::RequestAuthPurpose purpose = cch::mcp::RequestAuthPurpose::Operation) override;

    /// pi `onUnauthorized`: rotate a 401-rejected credential through the same
    /// shared refresh and report whether the request may retry once. A waiter
    /// cancelled during the refresh settles immediately; the exchange keeps
    /// running and is persisted.
    [[nodiscard]] support::AsyncResult<bool> recover_unauthorized(
            std::stop_token stop_token, std::string rejected_authorization) override;

    /// Resolve once no refresh is running (pi provider `settled()`): shutdown
    /// and tests use it so a rotated token answered after close is saved.
    [[nodiscard]] support::AsyncResult<void> settled();

private:
    struct State;

    std::shared_ptr<McpAuthStore> store_;
    std::string server_name_;
    std::string server_url_;
    std::shared_ptr<cch::mcp::OAuthProvider> provider_;
    std::shared_ptr<State> state_;
};

} // namespace cch::coding_agent::mcp
