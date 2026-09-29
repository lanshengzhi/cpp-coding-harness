#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/AsyncResult.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

class McpToolBinding;

/// The MCP Host's call-approval policy, bound to the Agent's existing
/// before-tool-call hook (issue #842; ADR 0018's awaitable policy seam, ADR
/// 0065's assignment of approval-policy hook binding to `SessionFactory`).
///
/// The policy is name-driven, not registration-driven: the hook is consulted
/// for every resolved call, decides from the Qualified Tool Name's Server Id,
/// and therefore treats a tool published after the Agent was constructed
/// exactly like a built-in one.
///
/// Everything that is not a call to an `approval: "ask"` Upstream MCP Server is
/// allowed untouched. That is deliberate: the built-in `read`/`write`/`edit`/
/// `bash` set and an `allow` server must not change behavior because a
/// third-party namespace exists.
///
/// An `ask` call is **fail-closed** here. Issue #843 replaces the refusal with
/// the real prompt; until it lands, the call is blocked with an explicit
/// reason and never silently treated as `allow`.
class McpToolApprovalPolicy {
public:
    /// `binding` answers which Server Id a Qualified Tool Name was published
    /// under and what that Server Id's configured approval is. A null binding
    /// blocks nothing, which is the same outcome as never having configured a
    /// server.
    explicit McpToolApprovalPolicy(std::shared_ptr<McpToolBinding> binding);

    /// The `cch::agent::BeforeToolCallHook` value to install into
    /// `AsyncAgentOptions::before_tool_call`.
    [[nodiscard]] agent::BeforeToolCallHook make_hook();

private:
    /// The reason an `ask` call is refused until the prompt lands. One
    /// template with one bounded, redacted interpolation — the session's own
    /// registered name, never Upstream-supplied text.
    [[nodiscard]] static std::string interim_block_reason(std::string_view qualified_name);

    std::shared_ptr<McpToolBinding> binding_;
};

} // namespace cch::coding_agent::runtime
