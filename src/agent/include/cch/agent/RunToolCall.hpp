#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/AsyncResult.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace cch::agent {

class NestedToolCallRunner;

/// The outcome of one tool call executed through `run_tool_call` (pi `AgentToolCallOutcome`).
struct AgentToolCallOutcome {
    ai::ToolCallContent tool_call{};
    AsyncToolExecutionResult result{};
    bool is_error{false};
    /// Milliseconds `execute()` took, measured with a monotonic clock; absent when
    /// the tool was not executed (for example unknown tool, validation failure,
    /// or blocked by hook).
    std::optional<std::int64_t> duration_ms{std::nullopt};
};

/// Options for `run_tool_call` (pi `RunToolCallOptions`).
struct RunToolCallOptions {
    /// Tools the call resolves against; must outlive the execute coroutine.
    std::reference_wrapper<ToolRegistry> tools;
    /// Passed to the hooks as the message that issued the call.
    ai::AssistantMessage assistant_message{};
    /// Passed to the hooks as the current agent context.
    ai::AiContext context{};
    /// Optional hook invoked before tool execution; must outlive coroutine if non-null.
    BeforeToolCallHook* before_tool_call{nullptr};
    /// Optional hook invoked after tool execution; must outlive coroutine if non-null.
    AfterToolCallHook* after_tool_call{nullptr};
    /// Optional cancellation token.
    std::stop_token stop_token{};
    /// Callback used by streaming tools to publish partial updates.
    ToolUpdateSink on_update{};
    /// Optional nested call runner; must outlive coroutine if non-null.
    NestedToolCallRunner* nested_calls{nullptr};
};

/// pi `runToolCall`: run one tool call through the same steps as a model-issued
/// call: argument preparation (`prepareArguments`), schema validation,
/// `beforeToolCall`, execution, and `afterToolCall`. Emits no events and adds
/// no messages. Tools that call other tools use this so hooks (for example
/// permission checks) apply to those calls too.
///
/// Never rejects for tool failures: unknown tools, validation errors, blocked
/// calls, and thrown errors are returned as outcomes with `is_error = true`.
[[nodiscard]] support::AsyncResult<AgentToolCallOutcome> run_tool_call(
        ai::ToolCallContent tool_call, RunToolCallOptions options);

} // namespace cch::agent
