#pragma once

#include <cch/ai/Content.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

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
    util::JsonValue arguments;
    std::string raw_arguments;
};

struct AsyncToolExecutionResult {
    std::vector<ai::Content> content;
    std::optional<util::JsonValue> details;
    bool is_error{false};
    bool terminate{false};
};

/// Synchronous publication of one cumulative partial tool execution result.
/// A failure asks the tool to stop producing updates and propagate the error.
using ToolUpdateSink = std::move_only_function<
    util::ExpectedVoid(const AsyncToolExecutionResult&)>;

struct BeforeToolCallContext {
    ai::AssistantMessage assistant_message;
    ai::ToolCallContent tool_call;
    util::JsonValue args;
    ai::AiContext context;
};

struct BeforeToolCallResult {
    bool block{false};
    std::optional<std::string> reason;
};

struct AfterToolCallContext {
    ai::AssistantMessage assistant_message;
    ai::ToolCallContent tool_call;
    util::JsonValue args;
    AsyncToolExecutionResult result;
    bool is_error{false};
    ai::AiContext context;
};

struct AfterToolCallResult {
    std::optional<std::vector<ai::Content>> content;
    std::optional<util::JsonValue> details;
    std::optional<bool> is_error;
    std::optional<bool> terminate;
};

/// Core policy contracts are move-only awaitable callables. Context values
/// deliberately cross this suspension boundary by value, so a hook cannot
/// retain a reference to run-owned state after the invocation finishes.
using BeforeToolCallHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<BeforeToolCallResult>>(
        BeforeToolCallContext,
        std::stop_token)>;
using AfterToolCallHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<AfterToolCallResult>>(
        AfterToolCallContext,
        std::stop_token)>;

/// Named adapters retain ergonomic synchronous policy setup without adding a
/// synchronous Agent execution path. Cancellable forms receive the active
/// run's stop token; non-cancellable forms explicitly ignore it.
using SyncBeforeToolCallPolicy = std::move_only_function<
    util::Expected<BeforeToolCallResult>(BeforeToolCallContext)>;
using CancellableSyncBeforeToolCallPolicy = std::move_only_function<
    util::Expected<BeforeToolCallResult>(BeforeToolCallContext, std::stop_token)>;
using SyncAfterToolCallPolicy = std::move_only_function<
    util::Expected<AfterToolCallResult>(AfterToolCallContext)>;
using CancellableSyncAfterToolCallPolicy = std::move_only_function<
    util::Expected<AfterToolCallResult>(AfterToolCallContext, std::stop_token)>;

[[nodiscard]] BeforeToolCallHook adapt_sync_before_tool_call(
    SyncBeforeToolCallPolicy policy);
[[nodiscard]] BeforeToolCallHook adapt_sync_before_tool_call(
    CancellableSyncBeforeToolCallPolicy policy);
[[nodiscard]] AfterToolCallHook adapt_sync_after_tool_call(
    SyncAfterToolCallPolicy policy);
[[nodiscard]] AfterToolCallHook adapt_sync_after_tool_call(
    CancellableSyncAfterToolCallPolicy policy);

enum class ToolConcurrency {
    /// pi `executionMode: "sequential"`: a batch containing a call to this
    /// tool executes entirely through the sequential path (per-tool sequential
    /// override, ADR 0034 / #355).
    Exclusive,
    /// pi `executionMode: "parallel"` (the default when omitted): this tool
    /// can execute concurrently with other tool calls.
    ParallelSafe,
};

class AsyncAgentTool {
public:
    virtual ~AsyncAgentTool() = default;

    [[nodiscard]] virtual const ai::Tool& definition() const = 0;

    /// pi `ToolDefinition.promptSnippet` (`core/tools/*.ts`): the one-line
    /// model-visible summary rendered into the System Prompt's `Available
    /// tools` list (`core/system-prompt.ts` `buildSystemPrompt`).
    /// `std::nullopt` keeps the tool out of the list.
    [[nodiscard]] virtual std::optional<std::string> prompt_snippet() const {
        return std::nullopt;
    }

    /// pi `ToolDefinition.promptGuidelines`: guideline bullets appended to
    /// the System Prompt's Guidelines section, in declaration order.
    [[nodiscard]] virtual std::vector<std::string> prompt_guidelines() const {
        return {};
    }
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncToolExecutionResult>> execute(
        ToolInvocation invocation,
        std::stop_token stop_token) = 0;

    /// Execute with cumulative partial-result observation. Ordinary tools may
    /// keep implementing execute(); streaming tools override this method and
    /// propagate update-sink failures through the normal expected channel.
    /// Updates published after the returned awaitable completes are ignored.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<AsyncToolExecutionResult>>
    execute_with_updates(
        ToolInvocation invocation,
        std::stop_token stop_token,
        ToolUpdateSink update_sink) {
        (void)update_sink;
        co_return co_await execute(std::move(invocation), stop_token);
    }

    /** Ordinary tools are exclusive until their adapter proves concurrent execution is safe. */
    [[nodiscard]] virtual ToolConcurrency concurrency() const noexcept {
        return ToolConcurrency::Exclusive;
    }
};

} // namespace cch::agent
