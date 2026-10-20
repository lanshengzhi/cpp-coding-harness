#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).
// #1066 removes this alias.

#include <cch/mcp/HttpClient.hpp>

namespace cch::coding_agent::mcp {

using McpHttpClient = cch::mcp::HttpClient;
using McpHttpGetStreamOptions = cch::mcp::HttpGetStreamOptions;

} // namespace cch::coding_agent::mcp
