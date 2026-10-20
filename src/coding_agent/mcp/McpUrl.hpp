#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/Url.hpp>

namespace cch::coding_agent::mcp {

using McpParsedUrl = cch::mcp::ParsedUrl;
using cch::mcp::extract_host;

[[nodiscard]] inline McpParsedUrl parse_mcp_url(std::string_view value) { return cch::mcp::parse_url(value); }
[[nodiscard]] inline std::string extract_mcp_host(std::string_view value) { return cch::mcp::extract_host(value); }

} // namespace cch::coding_agent::mcp
