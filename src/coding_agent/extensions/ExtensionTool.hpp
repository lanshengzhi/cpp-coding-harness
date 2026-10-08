#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/agent/NestedToolCalls.hpp>
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

/// pi `ExtensionToolContext` slice an extension Tool execute needs: the
/// model-issued call id it runs under and the nested-call dispatcher
/// (`ctx.executeTool`) when the host provides one. `nested_calls` is a
/// non-owning pointer to the run's dispatcher, which outlives the execution.
/// `update_sink` is the run's live tool-update publication (pi's `onUpdate`):
/// long-running tools (MCP progress) stream partial results through it.
struct ExtensionToolContext {
    agent::NestedToolCallRunner* nested_calls{nullptr};
    std::string call_id{};
    agent::ToolUpdateSink update_sink{};
};

/// Move-only execute operation of one extension-provided Tool. `arguments` are
/// the already-validated call arguments (the Agent's JSON Schema validation
/// runs before the call reaches here); cancellation is resolved by the
/// extension into its own terminal outcome (spec #865, ADR 0042).
using ExtensionToolExecute = std::move_only_function<support::AsyncResult<ExtensionToolResult>(
        support::JsonValue arguments, std::stop_token stop_token)>;

/// pi `ctx.executeTool`-aware execute form: the same contract as
/// `ExtensionToolExecute`, plus the run's nested-call dispatcher for tools that
/// run other tools (codemode scripts). A Tool sets this instead of `execute`
/// when it needs the context; the registry prefers it.
using ExtensionToolContextExecute = std::move_only_function<support::AsyncResult<ExtensionToolResult>(
        support::JsonValue arguments, ExtensionToolContext context, std::stop_token stop_token)>;

/// One Tool contributed by an Extension Tool Source (spec #865): the passive
/// model-facing descriptor, the prompt metadata, the concurrency policy, and
/// the move-only execute operation. The loader's runner converts this value
/// into a `cch::agent::Tool`, so an extension Tool obeys the same Agent
/// execution semantics (hooks, scheduling, per-call failure isolation) as a
/// built-in Tool.
struct ExtensionTool {
    ai::Tool definition;
    /// pi `ToolDefinition.defaultActive` (default true): whether registration
    /// declares the tool to the model. `false` registers the tool inertly —
    /// the model does not see it until an explicit activation names it (pi's
    /// `--tools` selection, a later `setActiveTools`, or the MCP exposure
    /// activation). pi registers its `codemode` and `tool_search` tools with
    /// `defaultActive: false`; every other extension tool is declarable on
    /// registration.
    bool default_active{true};
    /// pi `executionMode`: the scheduling policy the Agent applies. Carried
    /// onto the Agent Tool unchanged.
    agent::ToolConcurrency concurrency{agent::ToolConcurrency::Exclusive};
    /// pi `ToolDefinition.promptSnippet`; absent keeps the tool out of the
    /// System Prompt's Available tools list.
    std::optional<std::string> prompt_snippet;
    /// pi `ToolDefinition.promptGuidelines`; appended in declaration order.
    std::vector<std::string> prompt_guidelines;
    ExtensionToolExecute execute;
    /// Set instead of `execute` by a Tool that runs other tools through the
    /// nested-call seam; the registry prefers this form when present.
    ExtensionToolContextExecute context_execute;
};

} // namespace cch::coding_agent::extensions
