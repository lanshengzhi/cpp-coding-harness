#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/Auth.hpp>

namespace cch::coding_agent::mcp {

using cch::mcp::AuthEvent;
using cch::mcp::AuthEventVariant;
using cch::mcp::AuthInteraction;
using cch::mcp::AuthNotifyHook;
using cch::mcp::AuthProgress;
using cch::mcp::AuthPrompt;
using cch::mcp::AuthPromptHook;
using cch::mcp::AuthUrl;
using cch::mcp::OAuthCredential;
using cch::mcp::format_oauth_reauthenticate_message;
using cch::mcp::mcp_namespace;
using cch::mcp::mcp_oauth_provider_id;

} // namespace cch::coding_agent::mcp
