#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).
// #1066 removes this alias.

#include <cch/mcp/Client.hpp>

namespace cch::coding_agent::mcp {

using cch::mcp::RequestOptions;
using McpServerConnection = cch::mcp::Client;

} // namespace cch::coding_agent::mcp
