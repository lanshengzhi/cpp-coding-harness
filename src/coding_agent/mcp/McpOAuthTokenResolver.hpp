#pragma once

#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpRequestAuthSource.hpp"

#include <cch/support/AsyncResult.hpp>

#include <map>
#include <memory>
#include <string>

namespace cch::coding_agent::mcp {

/// `mcp-auth.json`-backed OAuth resolution for one MCP server (spec #882,
/// ticket #884; the store replaced #875's `auth.json` reuse). Each call loads
/// the server's state, and when it is within the same five-minute validity
/// margin the Models runtime uses, refreshes it and persists the rotated state
/// back before the request continues. A missing state, a state without tokens,
/// or a failed refresh is an explicit re-login error — never a silent
/// unauthenticated request, and never a retry loop.
class McpOAuthTokenResolver final : public McpRequestAuthSource {
public:
    McpOAuthTokenResolver(std::shared_ptr<McpAuthStore> store,
            std::string server_name,
            std::string server_url,
            std::shared_ptr<McpOAuthProvider> provider);

    McpOAuthTokenResolver(McpOAuthTokenResolver&&) noexcept;
    McpOAuthTokenResolver& operator=(McpOAuthTokenResolver&&) noexcept;
    ~McpOAuthTokenResolver() override;
    McpOAuthTokenResolver(const McpOAuthTokenResolver&) = delete;
    McpOAuthTokenResolver& operator=(const McpOAuthTokenResolver&) = delete;

    [[nodiscard]] support::AsyncResult<std::map<std::string, std::string>> current_headers() override;

private:
    std::shared_ptr<McpAuthStore> store_;
    std::string server_name_;
    std::string server_url_;
    std::shared_ptr<McpOAuthProvider> provider_;
};

} // namespace cch::coding_agent::mcp
