#pragma once

#include <cch/support/AsyncResult.hpp>

#include <map>
#include <stop_token>
#include <string>

namespace cch::mcp {

enum class RequestAuthPurpose {
    Operation,
    SessionClose,
};

/// Supplies the per-request authentication headers for one MCP server. The
/// headers are resolved for each request so a rotated or expired credential is
/// never cached across calls. A missing credential or a failed refresh is an
/// explicit error, never an empty header set: the transport has no
/// unauthenticated fallback.
class RequestAuthSource {
public:
    virtual ~RequestAuthSource() = default;

    [[nodiscard]] virtual support::AsyncResult<std::map<std::string, std::string>> current_headers(
            std::stop_token stop_token, RequestAuthPurpose purpose) = 0;

    /// pi `AuthProvider.onUnauthorized`: recover once from a credential the
    /// server rejected with 401. `rejected_authorization` is the header value
    /// the failed request carried (`Bearer <access>`). Completes with whether
    /// the request should be retried with freshly resolved headers; a source
    /// without a recoverable credential returns false, keeping the explicit
    /// re-login error. A cancelled waiter settles immediately while a shared
    /// refresh it started keeps running (ticket #1189).
    [[nodiscard]] virtual support::AsyncResult<bool> recover_unauthorized(
            std::stop_token stop_token, std::string rejected_authorization) = 0;
};

} // namespace cch::mcp
