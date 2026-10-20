#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/Pkce.hpp>

namespace cch::coding_agent::mcp {

using cch::mcp::AuthorizationInput;
using cch::mcp::base64_encode;
using cch::mcp::base64url_encode;
using cch::mcp::create_oauth_state;
using cch::mcp::extract_account_id;
using cch::mcp::generate_pkce;
using cch::mcp::parse_authorization_input;
using cch::mcp::parse_query_pairs;
using cch::mcp::PkcePair;
using cch::mcp::sha256_digest;
using cch::mcp::url_query_decode;
using cch::mcp::url_query_encode;

} // namespace cch::coding_agent::mcp
