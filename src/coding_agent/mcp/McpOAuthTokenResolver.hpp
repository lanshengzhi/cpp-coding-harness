#pragma once

#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpRequestAuthSource.hpp"

#include <cch/ai/CredentialStore.hpp>
#include <cch/support/AsyncResult.hpp>

#include <memory>
#include <string>

namespace cch::coding_agent::mcp {

/// AuthStorage-backed OAuth resolution for one MCP server (spec #865, ticket
/// #875). Each call reads the shared `auth.json` credential through the
/// credential store and, when the credential is within the same five-minute
/// validity margin the Models runtime uses, refreshes it inside the same
/// `CredentialStore::modify` transaction. That transaction re-reads the file
/// under the whole-file lock and runs on AuthStorage's own execution context, so
/// the request-time refresh stays off the caller's Runtime loop and a credential
/// written by another process (or another AuthStorage instance) is observed. A
/// missing credential, a non-OAuth record, or a failed refresh is an explicit
/// re-login error — never a silent unauthenticated request, and never a retry
/// loop.
class McpOAuthTokenResolver final : public McpRequestAuthSource {
public:
    McpOAuthTokenResolver(std::shared_ptr<ai::CredentialStore> credentials,
            std::string provider_id,
            std::string server_name,
            std::shared_ptr<McpOAuthProvider> provider);

    McpOAuthTokenResolver(McpOAuthTokenResolver&&) noexcept;
    McpOAuthTokenResolver& operator=(McpOAuthTokenResolver&&) noexcept;
    ~McpOAuthTokenResolver() override;
    McpOAuthTokenResolver(const McpOAuthTokenResolver&) = delete;
    McpOAuthTokenResolver& operator=(const McpOAuthTokenResolver&) = delete;

    [[nodiscard]] support::AsyncResult<std::map<std::string, std::string>> current_headers() override;

private:
    std::shared_ptr<ai::CredentialStore> credentials_;
    std::string provider_id_;
    std::string server_name_;
    std::shared_ptr<McpOAuthProvider> provider_;
};

} // namespace cch::coding_agent::mcp
