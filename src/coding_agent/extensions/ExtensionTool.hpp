#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

/// One terminal outcome from an extension-provided Tool. This is the
/// extension-side result shape: model-visible content, optional structured
/// details, and the error flag. Verbosity and termination are Agent execution
/// policies, so an extension Tool does not set them; the runner leaves them at
/// the Agent Tool defaults.
struct ExtensionToolResult {
    std::vector<ai::Content> content;
    std::optional<support::JsonValue> details;
    bool is_error{false};
};

/// Move-only execute operation of one extension-provided Tool. `arguments` are
/// the already-validated call arguments (the Agent's JSON Schema validation
/// runs before the call reaches here); cancellation is resolved by the
/// extension into its own terminal outcome (spec #865, ADR 0042).
using ExtensionToolExecute = std::move_only_function<support::AsyncResult<ExtensionToolResult>(
        support::JsonValue arguments, std::stop_token stop_token)>;

/// One Tool contributed by an Extension Tool Source (spec #865): the passive
/// model-facing descriptor, the prompt metadata, the concurrency policy, and
/// the move-only execute operation. The loader's runner converts this value
/// into a `cch::agent::Tool`, so an extension Tool obeys the same Agent
/// execution semantics (hooks, scheduling, per-call failure isolation) as a
/// built-in Tool.
struct ExtensionTool {
    ai::Tool definition;
    /// pi `executionMode`: the scheduling policy the Agent applies. Carried
    /// onto the Agent Tool unchanged.
    agent::ToolConcurrency concurrency{agent::ToolConcurrency::Exclusive};
    /// pi `ToolDefinition.promptSnippet`; absent keeps the tool out of the
    /// System Prompt's Available tools list.
    std::optional<std::string> prompt_snippet;
    /// pi `ToolDefinition.promptGuidelines`; appended in declaration order.
    std::vector<std::string> prompt_guidelines;
    ExtensionToolExecute execute;
};

} // namespace cch::coding_agent::extensions
