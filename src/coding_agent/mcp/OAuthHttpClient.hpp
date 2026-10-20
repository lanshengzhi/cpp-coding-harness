#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/OAuthHttpClient.hpp>

namespace cch::coding_agent::mcp {

using cch::mcp::default_oauth_http_client;
using cch::mcp::OAuthHttpClient;
using cch::mcp::OAuthHttpResponse;

} // namespace cch::coding_agent::mcp
