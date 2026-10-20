#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).
// #1066 migrates the application off these aliases.

#include <cch/mcp/StdioClient.hpp>

namespace cch::coding_agent::mcp {

using McpStdioClient = cch::mcp::StdioClient;

} // namespace cch::coding_agent::mcp
