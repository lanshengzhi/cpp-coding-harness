#pragma once

#include <cch/ai/Content.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::agent {

struct ToolInvocation {
    std::string call_id;
    std::string name;
    support::JsonValue arguments;
    std::string raw_arguments;
};

struct AsyncToolExecutionResult {
    std::vector<ai::Content> content;
    std::optional<support::JsonValue> details;
    bool is_error{false};
    bool terminate{false};
};

/// Synchronous publication of one cumulative partial tool execution result.
/// A failure asks the tool to stop producing updates and propagate the error.
using ToolUpdateSink = std::move_only_function<
    support::ExpectedVoid(const AsyncToolExecutionResult&)>;

struct BeforeToolCallContext {
    ai::AssistantMessage assistant_message;
    ai::ToolCallContent tool_call;
    support::JsonValue args;
    ai::AiContext context;
};

struct BeforeToolCallResult {
    bool block{false};
    std::optional<std::string> reason;
};

struct AfterToolCallContext {
    ai::AssistantMessage assistant_message;
    ai::ToolCallContent tool_call;
    support::JsonValue args;
    AsyncToolExecutionResult result;
    bool is_error{false};
    ai::AiContext context;
};

struct AfterToolCallResult {
    std::optional<std::vector<ai::Content>> content;
    std::optional<support::JsonValue> details;
    std::optional<bool> is_error;
    std::optional<bool> terminate;
};

/// Core policy contracts are move-only awaitable callables. Context values
/// deliberately cross this suspension boundary by value, so a hook cannot
/// retain a reference to run-owned state after the invocation finishes.
using BeforeToolCallHook = std::move_only_function<
    support::AsyncResult<BeforeToolCallResult>(
        BeforeToolCallContext,
        std::stop_token)>;
using AfterToolCallHook = std::move_only_function<
    support::AsyncResult<AfterToolCallResult>(
        AfterToolCallContext,
        std::stop_token)>;

enum class ToolConcurrency {
    /// pi `executionMode: "sequential"`: a batch containing a call to this
    /// tool executes entirely through the sequential path (per-tool sequential
    /// override, ADR 0034 / #355).
    Exclusive,
    /// pi `executionMode: "parallel"` (the default when omitted): this tool
    /// can execute concurrently with other tool calls.
    ParallelSafe,
};

/// Terminal result of one Agent Tool execute operation, delivered through a
/// typed `AsyncResult` (ADR 0040 §Agent Tool).
using ToolExecuteResult = cch::support::AsyncResult<AsyncToolExecutionResult>;

/// One move-only execute operation: accepts an invocation, the active run's
/// stop token, and an optional cumulative Tool Update sink, and returns a
/// typed `AsyncResult` consumed exactly once. Ordinary tools ignore the sink;
/// streaming tools publish cumulative partial results through it before they
/// complete the terminal outcome. Updates published after the terminal
/// completion are ignored by the executor.
using ToolExecute = std::move_only_function<
    ToolExecuteResult(
        ToolInvocation invocation,
        std::stop_token stop_token,
        ToolUpdateSink update_sink)>;

/// Passive Agent Tool (ADR 0040 §Agent Tool): the model-facing descriptor
/// (`ai::Tool`), prompt metadata (pi `ToolDefinition.promptSnippet` /
/// `promptGuidelines`), the concurrency policy, and one move-only execute
/// operation. Tools are aggregate-friendly values owned by the registry;
/// the move-only execute operation makes the Tool itself move-only.
struct Tool {
    ai::Tool definition{};
    /// pi `ToolDefinition.promptSnippet` (`core/tools/*.ts`): the one-line
    /// model-visible summary rendered into the System Prompt's `Available
    /// tools` list. `std::nullopt` keeps the tool out of the list.
    std::optional<std::string> prompt_snippet{};
    /// pi `ToolDefinition.promptGuidelines`: guideline bullets appended to
    /// the System Prompt's Guidelines section, in declaration order.
    std::vector<std::string> prompt_guidelines{};
    ToolConcurrency concurrency{ToolConcurrency::Exclusive};
    ToolExecute execute;
};

} // namespace cch::agent
