#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).
// #1066 migrates the application off these aliases; cch_mcp owns the
// definitions and never depends on the application.

#include <cch/mcp/Protocol.hpp>

namespace cch::coding_agent::mcp {

using cch::mcp::kClientName;
using cch::mcp::kClientVersion;
using cch::mcp::kMaxListPages;
using cch::mcp::kMaxToolNameLength;
using cch::mcp::kProtocolVersion;
using cch::mcp::kSupportedProtocolVersions;

namespace detail = cch::mcp::detail;

// Transitional spellings kept for the application and its tests until #1066.
inline constexpr std::string_view kMcpProtocolVersion = cch::mcp::kProtocolVersion;
inline constexpr int kMcpMaxListPages = cch::mcp::kMaxListPages;
inline constexpr std::size_t kMcpMaxToolNameLength = cch::mcp::kMaxToolNameLength;

} // namespace cch::coding_agent::mcp
