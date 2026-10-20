#pragma once

// Application registration surface for one stdio MCP server: the generic
// transport descriptor plus the `mcp.json` entry fields (exposure,
// description, per-tool overrides and timeout) the application owns.

#include "coding_agent/mcp/McpServerConfigBase.hpp"

#include <cch/mcp/StdioServerConfig.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace cch::coding_agent::mcp {

struct McpStdioServerConfig : McpServerConfigBase, cch::mcp::StdioServerConfig {};

} // namespace cch::coding_agent::mcp
