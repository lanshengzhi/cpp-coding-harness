#pragma once

#include <cch/ai/Message.hpp>
#include <cch/chord/Json.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Task.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::durable {

/// The built-in nested-tool task kind (frozen pi `ToolTask.definition.name`).
inline constexpr std::string_view kToolTaskKind{"pi.tool"};

/// One harness diagnostic of a nested call's error result (frozen pi `ToolDiagnostic`).
struct NestedCallDiagnostic {
    /// The frozen diagnostic code, such as `aborted`, `interrupted`, or `tool_unavailable`.
    std::string code{};
    std::string message{};

    [[nodiscard]] bool operator==(const NestedCallDiagnostic&) const = default;
};

/// One nested call's stored result, as its caller reads it (frozen pi
/// `NestedToolExecutionResult`). The caller maps it to the value or rejection its
/// script sees; `content` replaces the frozen `structuredOutput`.
struct NestedCallResult {
    bool is_error{false};
    std::vector<ai::Content> content{};
    std::optional<support::JsonValue> details{std::nullopt};
    std::optional<std::int64_t> duration_ms{std::nullopt};
    /// The harness diagnostics of an error result; a cancelled or interrupted
    /// call reports its reason here instead of model-visible text.
    std::vector<NestedCallDiagnostic> diagnostics{};

    [[nodiscard]] bool operator==(const NestedCallResult&) const = default;
};

/// The nested calls of one calling task: a task-scoped keyed family holding, per
/// call key, the nested task's ID and — once the call ends — its result (frozen
/// pi `NestedCallDoc`). One member per call, so no commit rewrites the others;
/// the members retire with the calling task.
[[nodiscard]] DocumentDefinition nested_call_document();

/// Decode a stored nested-call result (frozen pi `NestedToolExecutionResult`).
[[nodiscard]] support::Expected<NestedCallResult> decode_nested_result(const chord::Json& result);

/// Create or reattach nested call `key` of `parent` in one commit (frozen pi
/// `startNestedCall`): the child task, its caller-owned member and the recorded
/// task identity commit together, so a rerun finds the call it already made.
/// A key that names another tool or other arguments is rejected.
[[nodiscard]] support::Expected<TaskId> start_nested_call(TaskRuntime& runtime,
        TaskId parent,
        std::string_view parent_call_id,
        std::string_view key,
        const ai::ToolCallContent& call);

/// The message a nested call hands its caller, with `call_id` and `name` set
/// (frozen pi `executeTool` returning the stored result).
[[nodiscard]] ai::ToolResultMessage nested_result_message(
        NestedCallResult result, std::string call_id, std::string name);

/// The built-in `pi.tool` definition for a nested call created through
/// `TaskRuntime::task_definition` + `Transaction::create_task` (frozen pi
/// `ToolTask` with a `nested` input).
///
/// The `call` phase resolves the tool among the invocation's callable tools,
/// runs the `beforeTool` hooks, records the execution intent, runs the tool, lets
/// `afterTool` replace the result, and settles the result into the caller's
/// nested-call member. The `execute` phase is reached only by recovery and
/// applies the replay rule (rerun only when both the recorded and the current
/// policy are safe). The `abort` handler settles an aborted result, so a
/// cancelled nested call reports into the member its caller is waiting on.
///
/// `session` must outlive every invocation of the returned definition; the
/// Harness owns it for the definition's whole lifetime.
[[nodiscard]] support::Expected<TaskDefinition> nested_tool_task(Session& session);

} // namespace cch::durable
