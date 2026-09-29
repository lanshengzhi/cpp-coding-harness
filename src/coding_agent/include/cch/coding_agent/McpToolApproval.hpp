#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>
#include <stop_token>
#include <string>

namespace cch::coding_agent {

/// One call to an `approval: "ask"` Upstream MCP Server, awaiting the user's
/// call-time authorization (issue #843, spec #833 story 22).
///
/// Every field is what the prompt needs to be answerable, and nothing else: the
/// call's own identity, the Server it targets, and the exact arguments that
/// would be sent upstream if the user consents. The arguments are the prepared
/// (`resolve_tool_call` output) arguments, because that is what the call would
/// put on the wire — the consent is about the effective call, not the model's
/// raw text.
struct McpToolApprovalRequest {
    /// Server Id of the Upstream MCP Server the call targets.
    std::string server_id{};
    /// `mcp__<Server Id>__<tool>` — the name in the model's tool payload and
    /// the name the user is asked to authorize.
    std::string qualified_tool_name{};
    /// The Upstream's own tool name, as its `tools/list` entry spelled it.
    std::string tool_name{};
    /// The call's arguments, as compact JSON, bounded and redacted.
    std::string arguments_json{};
};

/// The user's answer to one call-approval prompt.
///
/// `Declined` and `Cancelled` both block the call, and they are distinct
/// because a dismissal is not a decision: nothing is remembered, and the next
/// call to the same server asks again (the project-trust prompt's own
/// session-only split, `McpServerTrustAnswer`).
enum class McpToolApprovalAnswer {
    /// Run this one call.
    Allowed,
    /// Refuse this one call, with the refusal recorded as the user's decision.
    Declined,
    /// Dismissed the prompt: refuse this one call and remember nothing.
    Cancelled,
};

/// The frontend's call-time approval prompt for an `approval: "ask"` Upstream
/// MCP Server. It answers for one call alone: there is no "always allow", and
/// no decision is persisted here (spec #833 keeps call-time decisions
/// per-call).
///
/// A session with no such prompt cannot obtain consent, so the call-approval
/// policy refuses the call rather than treating the missing prompt as an
/// answer — the same fail-closed rule the first-enable trust gate applies to
/// a non-interactive session.
using McpToolApprovalPrompter =
        std::move_only_function<support::AsyncResult<McpToolApprovalAnswer>(McpToolApprovalRequest, std::stop_token)>;

} // namespace cch::coding_agent
