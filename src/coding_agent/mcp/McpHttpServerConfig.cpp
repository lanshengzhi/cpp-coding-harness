// Application-side registration gate forwarding to the cch_mcp Owner
// Interface (extraction #1065): the TLS-only boundary is currently shared,
// and #1066's transport slice owns the behavior change.

#include "coding_agent/mcp/McpHttpServerConfig.hpp"

namespace cch::coding_agent::mcp {

support::ExpectedVoid validate_mcp_http_server_config(const McpHttpServerConfig& config) {
    return cch::mcp::validate_http_server_config(config);
}

} // namespace cch::coding_agent::mcp
