#include "coding_agent/runtime/McpToolApprovalPolicy.hpp"

#include "coding_agent/runtime/McpToolBinding.hpp"

#include "support/Json.hpp"

#include <cch/support/OutputLimiter.hpp>

#include <format>
#include <utility>

namespace cch::coding_agent::runtime {
namespace {

/// The bounded, redacted refusal an `ask` call gets when the user does not
/// consent — for any of the four non-consent outcomes, which differ only in
/// the trailing clause. The Qualified Tool Name is the session's own registered
/// name, never Upstream-supplied text, so there is nothing here to bound beyond
/// habit — the bound is what keeps that true if the sentence ever grows an
/// interpolated detail.
constexpr std::size_t kMaxReasonBytes{1024};

/// The largest argument rendering an approval prompt carries (issue #843).
///
/// The prompt is about the exact call, so the arguments are shown in full up
/// to this bound; a call whose prepared arguments render larger is still
/// answered, with the rendering cut and marked. This is a containment bound on
/// a third party's payload (recorded in `docs/runtime-capacities.md`), not a
/// second product output limit.
constexpr std::size_t kMaxArgumentsBytes{64 * 1024};

/// The clause each non-consent outcome adds to the shared refusal.
constexpr std::string_view kNoPrompterBecause =
        "this session has no way to ask for call approval, so the call was refused rather than run";
constexpr std::string_view kDeclinedBecause = "the user declined it, so it was not run";
constexpr std::string_view kCancelledBecause =
        "the user dismissed the approval prompt, so it was not run and no decision was recorded";
constexpr std::string_view kPromptFailedBecause = "the approval prompt could not be answered, so it was refused "
                                                  "rather than run";

} // namespace

McpToolApprovalPolicy::McpToolApprovalPolicy(std::shared_ptr<McpToolBinding> binding, McpToolApprovalPrompter prompter)
    : binding_(std::move(binding)), prompter_(std::move(prompter)) {}

std::string McpToolApprovalPolicy::block_reason(std::string_view qualified_name, std::string_view because) {
    return support::bounded_redacted_text(
            std::format("call to '{}' was not approved: {}", qualified_name, because), kMaxReasonBytes, "...");
}

McpToolApprovalRequest McpToolApprovalPolicy::make_request(
        const McpPublishedTool& tool, const support::JsonValue& arguments) {
    // The prepared arguments are the call's real arguments, so this is what the
    // user is authorizing. Serialization cannot fail for a `JsonValue` this
    // package already produced; the fallback keeps a rendering out of a refusal
    // if it ever does.
    auto rendered = support::write_json(arguments);
    const std::string arguments_json =
            rendered ? support::bounded_redacted_text(
                               std::move(*rendered), kMaxArgumentsBytes, "\n… arguments truncated")
                     : std::string{"(arguments could not be rendered)"};
    return McpToolApprovalRequest{
            .server_id = tool.server_id,
            .qualified_tool_name = tool.qualified_name,
            .tool_name = tool.tool_name,
            .arguments_json = arguments_json,
    };
}

[[nodiscard]] agent::BeforeToolCallHook McpToolApprovalPolicy::make_hook() {
    // The binding and the prompter travel by value into the hook: a policy hook
    // outlives the value that built it, and a `this` capture would be exactly
    // the dangling reference the hook contract forbids (ADR 0040). The prompter
    // is a `move_only_function` the hook is called with, so it travels behind a
    // `shared_ptr` and each call borrows it rather than consuming it.
    std::shared_ptr<McpToolApprovalPrompter> prompter;
    if (prompter_) {
        prompter = std::make_shared<McpToolApprovalPrompter>(std::move(prompter_));
    }
    return [binding = binding_, prompter = std::move(prompter)](agent::BeforeToolCallContext context,
                   std::stop_token stop_token) -> support::AsyncResult<agent::BeforeToolCallResult> {
        const auto allow = [] { return agent::BeforeToolCallResult{}; };
        if (binding == nullptr) {
            // No binding means no published MCP tool, so no call can name an
            // `ask` server. The built-in set must never be blocked by a
            // third-party namespace it cannot even see.
            return support::AsyncResult<agent::BeforeToolCallResult>(
                    support::Expected<agent::BeforeToolCallResult>{allow()});
        }
        const auto tool = binding->publication_for(context.tool_call.name);
        if (!tool || binding->approval_for(tool->server_id) != McpServerApproval::Ask) {
            // A built-in tool name, and every `allow` server, answer inline: the
            // ordinary path never suspends on a third-party prompt seam.
            return support::AsyncResult<agent::BeforeToolCallResult>(
                    support::Expected<agent::BeforeToolCallResult>{allow()});
        }
        const auto qualified_name = tool->qualified_name;
        if (!prompter) {
            // A non-interactive session cannot obtain consent. Failing closed
            // is the whole of the missing capability: an `ask` call is refused
            // rather than run because nobody could say yes (the trust gate's
            // `PromptUnavailable` precedent).
            return support::AsyncResult<agent::BeforeToolCallResult>(
                    support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{
                            .block = true,
                            .reason = block_reason(qualified_name, kNoPrompterBecause),
                    }});
        }
        // One call, one question, one answer: the producer asks exactly once
        // and completes exactly once, so the hook cannot storm the prompt.
        const auto request = make_request(*tool, context.args);
        // The reason is built from a copy of the name, because the producer
        // completes the operation after this call returns.
        const auto refused = [qualified_name](std::string_view because) {
            return support::Expected<agent::BeforeToolCallResult>{
                    agent::BeforeToolCallResult{.block = true, .reason = block_reason(qualified_name, because)}};
        };
        return support::AsyncResult<agent::BeforeToolCallResult>(support::AsyncProducer<agent::BeforeToolCallResult,
                support::Error>{[prompter, request = std::move(request), refused, stop_token](
                                        support::AsyncCompletion<agent::BeforeToolCallResult, support::Error>
                                                completion) mutable noexcept {
            (*prompter)(std::move(request), stop_token)
                    .start([refused, completion = std::move(completion)](
                                   std::expected<McpToolApprovalAnswer, support::Error> answer) mutable noexcept {
                        if (!answer) {
                            completion(refused(kPromptFailedBecause));
                            return;
                        }
                        if (*answer == McpToolApprovalAnswer::Allowed) {
                            completion(support::Expected<agent::BeforeToolCallResult>{agent::BeforeToolCallResult{}});
                            return;
                        }
                        completion(refused(
                                *answer == McpToolApprovalAnswer::Declined ? kDeclinedBecause : kCancelledBecause));
                    });
        }});
    };
}

} // namespace cch::coding_agent::runtime
