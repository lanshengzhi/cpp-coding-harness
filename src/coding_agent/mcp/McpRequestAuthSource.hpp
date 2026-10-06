#pragma once

#include <cch/support/AsyncResult.hpp>

#include <map>
#include <string>

namespace cch::coding_agent::mcp {

/// Supplies the per-request authentication headers for one MCP server (spec
/// #865, ticket #875). The headers are resolved for each request so a rotated
/// or expired credential is never cached across calls. A missing credential or
/// a failed refresh is an explicit error, never an empty header set: the
/// transport has no unauthenticated fallback.
class McpRequestAuthSource {
public:
    virtual ~McpRequestAuthSource() = default;

    [[nodiscard]] virtual support::AsyncResult<std::map<std::string, std::string>> current_headers() = 0;
};

} // namespace cch::coding_agent::mcp
