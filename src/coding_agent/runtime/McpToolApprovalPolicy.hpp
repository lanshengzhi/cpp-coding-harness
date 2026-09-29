#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/coding_agent/McpToolApproval.hpp>
#include <cch/coding_agent/McpToolBinding.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

class McpToolBinding;

/// The MCP Host's call-approval policy, bound to the Agent's existing
/// before-tool-call hook (issues #842 and #843; ADR 0018's awaitable policy
/// seam, ADR 0065's assignment of approval-policy hook binding to
/// `SessionFactory`).
///
/// The policy is name-driven, not registration-driven: the hook is consulted
/// for every resolved call, decides from the Qualified Tool Name's Server Id,
/// and therefore treats a tool published after the Agent was constructed
/// exactly like a built-in one.
///
/// Everything that is not a call to an `approval: "ask"` Upstream MCP Server is
/// allowed untouched, and the decision is delivered without suspending. That is
/// deliberate: the built-in `read`/`write`/`edit`/`bash` set and an `allow`
/// server must not change behavior — not even by a scheduling hop — because a
/// third-party namespace exists.
///
/// An `ask` call asks `prompter` and answers from it. Every other outcome is a
/// refusal, never a silent authorization: no prompter (a non-interactive
/// session), a declined answer, a dismissal, and a prompt that failed all
/// block the call with their own bounded reason. One refusal is one failed tool
/// call (ADR 0008), never a session failure.
class McpToolApprovalPolicy {
public:
    /// `binding` answers which Server Id a Qualified Tool Name was published
    /// under and what that Server Id's configured approval is. A null binding
    /// blocks nothing, which is the same outcome as never having configured a
    /// server.
    ///
    /// `prompter` is the frontend's call-approval prompt. Constructed with an
    /// empty prompter the policy describes a non-interactive session, where an
    /// `ask` call is refused rather than run.
    explicit McpToolApprovalPolicy(std::shared_ptr<McpToolBinding> binding, McpToolApprovalPrompter prompter = {});

    /// The `cch::agent::BeforeToolCallHook` value to install into
    /// `AsyncAgentOptions::before_tool_call`.
    [[nodiscard]] agent::BeforeToolCallHook make_hook();

private:
    /// The bounded, redacted refusal each non-consent outcome gets. One
    /// template with one bounded, redacted interpolation — the session's own
    /// registered name, never Upstream-supplied text — so a hostile server
    /// cannot widen the reason it is refused with.
    [[nodiscard]] static std::string block_reason(std::string_view qualified_name, std::string_view because);
    /// The `ask` server's prompt request, with the call's arguments rendered
    /// as bounded, redacted compact JSON.
    [[nodiscard]] static McpToolApprovalRequest make_request(
            const McpPublishedTool& tool, const support::JsonValue& arguments);

    std::shared_ptr<McpToolBinding> binding_;
    McpToolApprovalPrompter prompter_;
};

} // namespace cch::coding_agent::runtime
