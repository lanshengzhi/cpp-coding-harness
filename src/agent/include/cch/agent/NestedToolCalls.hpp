#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::agent {

/// pi `NESTED_CALL_LIMITS` (`core/nested-tool-calls.ts`): the bounds of the
/// nested-call record a model-issued tool call leaves on its result. Arguments
/// over the per-call or total size are omitted, calls beyond the count are
/// dropped, and the record is marked incomplete when any of that happens.
struct NestedCallLimits {
    std::size_t max_calls{256};
    std::size_t max_argument_bytes_per_call{8 * 1024};
    std::size_t max_argument_bytes_total{32 * 1024};
    std::size_t max_error_chars{500};
};

inline constexpr NestedCallLimits kNestedCallLimits{};

/// pi `NestedToolCallRecord`: one tool call a tool made through the nested-call
/// seam (for example a codemode script's `tools.*` call).
struct NestedToolCallRecord {
    std::string id{};
    std::string name{};
    /// pi `"unfinished" | "ok" | "error"`.
    std::string status{"unfinished"};
    std::optional<std::uint64_t> duration_ms{};
    /// The call arguments, omitted when they exceed the per-call or total cap.
    std::optional<support::JsonValue> arguments{};
    /// Present exactly when `arguments` was omitted for the size cap.
    std::optional<std::uint64_t> arguments_bytes{};
    std::optional<std::string> error{};
};

/// pi `NestedToolCalls`: the record a model-issued call leaves behind.
struct NestedToolCalls {
    std::vector<NestedToolCallRecord> calls{};
    bool complete{true};
};

/// Serialize a record to pi's `message.nestedCalls` JSON shape.
[[nodiscard]] support::JsonValue nested_calls_to_json(const NestedToolCalls& calls);

/// pi `NestedCallSummary`: what one model-issued call's nested calls leave on
/// its tool result message.
struct NestedCallSummary {
    /// pi `nestedCalls`; absent when no nested call was made.
    std::optional<NestedToolCalls> calls{};
};

/// pi `NestedToolCallHost`: the closures a `NestedToolCallRunner` resolves its
/// work through. The host closures are pinned to the Agent, not to a concrete
/// registry object, so every access resolves the live (Agent-owned) registry at
/// call time:
///   * `get_tools` — current callable tools (pi `_getCallableTools`),
///   * `is_sequential` — whether every nested call runs exclusively,
///   * `run_tool_call` — run one call through the tool pipeline,
///   * `emit` — deliver a `tool_execution_*` event carrying the parent id.
struct NestedToolCallHost {
    using GetTools = std::move_only_function<std::vector<ai::Tool>()>;
    using IsSequential = std::move_only_function<bool()>;
    using RunToolCall = std::move_only_function<support::AsyncResult<AsyncToolExecutionResult>(
            ToolInvocation, std::string parent_id, std::stop_token)>;
    using Emit = std::move_only_function<support::ExpectedVoid(const AgentLifecycleEvent&)>;

    GetTools get_tools;
    IsSequential is_sequential;
    RunToolCall run_tool_call;
    Emit emit;

    [[nodiscard]] bool valid() const { return get_tools && is_sequential && run_tool_call && emit; }
};

/// pi `NestedToolCallRunner`: runs the tool calls a tool makes while it runs.
/// The agent loop does not know about them; each nested call gets the id
/// `<callerId>/<n>`, runs through the pipeline, and is recorded (with pi's
/// limits) for the calling call's tool result. Calls below one model-issued
/// call share its recorder, so further nesting is recorded on the same result.
/// Nothing here runs until a tool invokes the seam.
class NestedToolCallRunner {
public:
    explicit NestedToolCallRunner(NestedToolCallHost host);

    NestedToolCallRunner(NestedToolCallRunner&&) noexcept;
    NestedToolCallRunner& operator=(NestedToolCallRunner&&) noexcept;
    NestedToolCallRunner(const NestedToolCallRunner&) = delete;
    NestedToolCallRunner& operator=(const NestedToolCallRunner&) = delete;
    ~NestedToolCallRunner();

    /// Run `name` on behalf of the call `caller_id`. The nested call gets the id
    /// `<caller_id>/<n>` and is recorded on that caller's result. A tool failure
    /// comes back as `is_error = true`; the operation itself never fails.
    [[nodiscard]] support::AsyncResult<AsyncToolExecutionResult> execute(
            std::string caller_id, std::string name, support::JsonValue arguments,
            std::stop_token signal);

    /// Remove and return the record of the nested calls `caller_id` made.
    [[nodiscard]] std::optional<NestedCallSummary> take_record(std::string_view caller_id);

    /// pi `host.getTools()`: the tools a nested call resolves against, read at
    /// call time from the live (Agent-owned) registry.
    [[nodiscard]] std::vector<ai::Tool> tools() const;

    /// pi `host.isSequential()`.
    [[nodiscard]] bool is_sequential() const;

    /// Drop every scope (pi `clear()`, run once per `agent_end`).
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::agent
