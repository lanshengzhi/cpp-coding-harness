#include "coding_agent/runtime/McpToolApprovalPolicy.hpp"

#include "coding_agent/runtime/McpToolBinding.hpp"

#include <cch/support/OutputLimiter.hpp>

#include <format>
#include <utility>

namespace cch::coding_agent::runtime {
namespace {

/// The bounded, redacted refusal an `ask` call gets until the call-approval
/// prompt lands. The Qualified Tool Name is the session's own registered name,
/// never Upstream-supplied text, so there is nothing here to bound beyond
/// habit — the bound is what keeps that true if the sentence ever grows an
/// interpolated detail.
constexpr std::size_t kMaxReasonBytes{1024};

} // namespace

McpToolApprovalPolicy::McpToolApprovalPolicy(std::shared_ptr<McpToolBinding> binding) : binding_(std::move(binding)) {}

std::string McpToolApprovalPolicy::interim_block_reason(std::string_view qualified_name) {
    return support::bounded_redacted_text(
            std::format("call-approval is not available yet: '{}' needs an explicit approval that this "
                        "build cannot ask for, so the call was refused rather than run",
                    qualified_name),
            kMaxReasonBytes,
            "...");
}

[[nodiscard]] agent::BeforeToolCallHook McpToolApprovalPolicy::make_hook() {
    // The binding travels by value into the hook: a policy hook outlives the
    // value that built it, and a `this` capture would be exactly the dangling
    // reference the hook contract forbids (ADR 0040).
    return [binding = std::move(binding_)](agent::BeforeToolCallContext context,
                   std::stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        const auto& owner = binding;
        if (owner == nullptr) {
            // No binding means no published MCP tool, so no call can name an
            // `ask` server. The built-in set must never be blocked by a
            // third-party namespace it cannot even see.
            return support::AsyncResult<agent::BeforeToolCallResult>(
                    support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}});
        }
        const auto server_id = owner->server_id_for(context.tool_call.name);
        if (!server_id || owner->approval_for(*server_id) != McpServerApproval::Ask) {
            return support::AsyncResult<agent::BeforeToolCallResult>(
                    support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}});
        }
        return support::AsyncResult<agent::BeforeToolCallResult>(
                support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{
                        .block = true,
                        .reason = interim_block_reason(context.tool_call.name),
                }});
    };
}

} // namespace cch::coding_agent::runtime
