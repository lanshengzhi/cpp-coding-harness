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
};

} // namespace cch::mcp
