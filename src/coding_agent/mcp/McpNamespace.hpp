#pragma once

// The one spelling of pi's MCP server namespace (`mcpNamespace`), shared by the
// tool-name namespace (McpConfigFile's collision check) and the OAuth
// credential-store provider id (mcp_oauth_provider_id), which are the same
// value by construction: `mcp__<server>` with every `-` replaced by `_`. Both
// live in cch_mcp (`cch::mcp::mcp_namespace`, extraction #1065); this header
// keeps only the application's resource-tool name constants.

#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

/// The Codex-compatible resource tool names (pi `core/mcp-servers.ts`). The
/// three tools reach every server with resources, so `runtime::is_mcp_tool_name`
/// recognizes them as MCP tools alongside `mcp__<server>__<tool>`.
inline constexpr std::string_view kListMcpResourcesTool = "list_mcp_resources";
inline constexpr std::string_view kListMcpResourceTemplatesTool = "list_mcp_resource_templates";
inline constexpr std::string_view kReadMcpResourceTool = "read_mcp_resource";

} // namespace cch::coding_agent::mcp
