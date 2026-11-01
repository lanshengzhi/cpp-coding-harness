#include "ToolTask.hpp"

#include "durable/AwaitResult.hpp"
#include "durable/JsonFields.hpp"
#include "durable/ToolContent.hpp"
#include "support/Json.hpp"

#include <cch/durable/Records.hpp>
#include <cch/durable/Workflow.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cch::durable {
namespace {

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

support::Error failure(std::string message) {
    return support::make_error(support::ErrorCode::Tool, std::move(message));
}

/// One nested call as its task input records it (frozen pi `ToolTaskInput` with
/// `kind: "nested"`).
struct NestedCall {
    TaskId parent{};
    std::string parent_call_id{};
    std::string key{};
    ai::ToolCallContent call{};
};

Json call_json(const ai::ToolCallContent& call, const Json& arguments) {
    Object value{{"type", "toolCall"}, {"id", call.id}, {"name", call.name}, {"arguments", arguments}};
    if (call.thought_signature) value.emplace("thoughtSignature", *call.thought_signature);
    return Json{std::move(value)};
}

/// The call as tool hooks see it: the arguments execution started from, and the
/// parent identity a nested call carries (frozen pi `ToolHookCall`).
Json hook_call_json(const NestedCall& call, const Json& arguments) {
    Json base = call_json(call.call, arguments);
    Object value = *base.as_object();
    value.emplace("parent", Json{Object{{"taskId", numeric(call.parent.value)}, {"callId", call.parent_call_id}}});
    return Json{std::move(value)};
}

support::Expected<NestedCall> parse_call(const Json& input) {
    const Json* kind = member(input, "kind");
    const Json* parent = member(input, "parent");
    const Json* parent_call_id = member(input, "parentCallId");
    const Json* key = member(input, "key");
    const Json* call = member(input, "call");
    if (kind == nullptr || kind->as_string() == nullptr || *kind->as_string() != "nested" || parent == nullptr ||
            parent_call_id == nullptr || parent_call_id->as_string() == nullptr || key == nullptr ||
            key->as_string() == nullptr || key->as_string()->empty() || call == nullptr)
        return std::unexpected(validation("A nested tool task needs a complete nested call input"));
    const Json* name = member(*call, "name");
    const Json* call_id = member(*call, "id");
    const auto parent_id = as_integer(*parent);
    if (name == nullptr || name->as_string() == nullptr || call_id == nullptr || call_id->as_string() == nullptr ||
            !parent_id.has_value())
        return std::unexpected(validation("A nested tool task needs a complete nested call input"));
    NestedCall parsed{.parent = TaskId{.value = *parent_id},
            .parent_call_id = *parent_call_id->as_string(),
            .key = *key->as_string()};
    parsed.call.id = *call_id->as_string();
    parsed.call.name = *name->as_string();
    const Json* arguments = member(*call, "arguments");
    if (arguments == nullptr || arguments->as_object() != nullptr) {
        parsed.call.arguments =
                arguments == nullptr ? support::JsonValue{support::JsonValue::object_t{}} : arguments->to_support();
    } else {
        parsed.call.arguments_valid = false;
    }
    if (const Json* signature = member(*call, "thoughtSignature");
            signature != nullptr && signature->as_string() != nullptr)
        parsed.call.thought_signature = *signature->as_string();
    return parsed;
}

/// The call's arguments as a JSON object; a call without arguments passes none.
Json arguments_of(const ai::ToolCallContent& call) {
    return call.arguments ? *Json::from_support(*call.arguments) : Json{Object{}};
}

bool same_arguments(const Json& left, const Json& right) {
    const auto left_text = support::canonical_json(left.to_support());
    const auto right_text = support::canonical_json(right.to_support());
    return left_text && right_text && *left_text == *right_text;
}

Json diagnostic_json(const NestedCallDiagnostic& diagnostic) {
    return Json{Object{{"severity", "error"}, {"code", diagnostic.code}, {"message", diagnostic.message}}};
}

/// The stored result of one nested call (frozen pi `NestedToolExecutionResult`).
Json result_json(const std::vector<ai::Content>& content,
        bool is_error,
        const std::optional<support::JsonValue>& details,
        std::optional<std::int64_t> duration_ms,
        const std::vector<NestedCallDiagnostic>& diagnostics) {
    Object value{{"isError", is_error}, {"content", content_json(content)}};
    if (details) value.emplace("details", *Json::from_support(*details));
    if (duration_ms) value.emplace("durationMs", numeric(*duration_ms));
    if (!diagnostics.empty()) {
        Array listed;
        for (const auto& diagnostic : diagnostics)
            listed.push_back(diagnostic_json(diagnostic));
        value.emplace("diagnostics", Json{std::move(listed)});
    }
    return Json{std::move(value)};
}

/// The registered tool a nested call names, or `nullptr` for a name this
/// invocation cannot reach (frozen pi `ToolTask.call` resolving `tool_unavailable`).
std::shared_ptr<ToolRegistration> resolve_tool(TaskRuntime& runtime, std::string_view name) {
    const auto& tools = runtime.callable_tools();
    const auto found =
            std::ranges::find_if(tools, [name](const auto& tool) { return tool && tool->declaration.name == name; });
    return found == tools.end() ? nullptr : *found;
}

/// Commit the nested call's result into its caller's member and settle the task
/// (frozen pi `ToolTask.settle` for a nested call).
support::ExpectedVoid settle(TaskRuntime& runtime, const NestedCall& call, Json result, std::string status) {
    return await_result(
            runtime.commit_result([call, result = std::move(result), status = std::move(status)](Transaction& tx,
                                          const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto document = tx.doc(nested_call_document(),
                        DocumentAddress{.task_id = call.parent, .key = call.key},
                        Json{Object{}});
                if (!document) return std::unexpected(document.error());
                if (auto set = document->set({chord::path_key("result")}, result); !set)
                    return std::unexpected(set.error());
                return std::optional<NextTaskState>{NextTaskState::terminal(
                        TaskOutcome{.status = status, .result = Json{Object{{"kind", "nested"}}}})};
            }));
}

/// Settle an error result the harness itself decided: its diagnostic is what the
/// caller reports, as a model-visible call would show it (frozen pi `harnessError`).
support::ExpectedVoid settle_harness_error(TaskRuntime& runtime,
        const NestedCall& call,
        std::string code,
        std::string message,
        std::string status = "completed") {
    return settle(runtime,
            call,
            result_json({},
                    true,
                    std::nullopt,
                    std::nullopt,
                    {NestedCallDiagnostic{.code = std::move(code), .message = std::move(message)}}),
            std::move(status));
}

} // namespace

support::Expected<TaskId> start_nested_call(TaskRuntime& runtime,
        TaskId parent,
        std::string_view parent_call_id,
        std::string_view key,
        const ai::ToolCallContent& call) {
    const TaskDefinition* definition = runtime.task_definition(kToolTaskKind);
    if (definition == nullptr) return std::unexpected(validation("The nested tool task is not registered"));
    const Json arguments = arguments_of(call);
    Json input{Object{{"kind", "nested"},
            {"parent", numeric(parent.value)},
            {"parentCallId", std::string{parent_call_id}},
            {"key", std::string{key}},
            {"call", call_json(call, arguments)}}};
    auto created = std::make_shared<support::Expected<TaskId>>(
            std::unexpected(validation("Nested call creation did not commit")));
    auto committed = await_result(runtime.commit(
            [definition,
                    input = std::move(input),
                    parent,
                    key = std::string{key},
                    name = call.name,
                    arguments,
                    created](Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto document =
                        tx.doc(nested_call_document(), DocumentAddress{.task_id = parent, .key = key}, Json{Object{}});
                if (!document) return std::unexpected(document.error());
                auto recorded = document->get({chord::path_key("taskId")});
                if (!recorded) return std::unexpected(recorded.error());
                if (recorded->has_value()) {
                    const auto recorded_id = (*recorded)->as_number();
                    if (recorded_id == nullptr) return std::unexpected(validation("A nested call recorded no task ID"));
                    auto existing = tx.task(TaskId{.value = static_cast<std::int64_t>(*recorded_id)});
                    if (!existing) return std::unexpected(existing.error());
                    if (!existing->has_value()) return std::unexpected(validation("A nested call names no task"));
                    auto recorded_input = parse_call((*existing)->input);
                    if (!recorded_input || recorded_input->call.name != name ||
                            !same_arguments(arguments_of(recorded_input->call), arguments))
                        return std::unexpected(failure(std::format(
                                "Nested call {} was already made with another tool or other arguments", key)));
                    *created = (*existing)->id;
                    return std::nullopt;
                }
                auto task = tx.create_task(*definition, input, TaskOptions{.owner = parent});
                if (!task) return std::unexpected(task.error());
                if (auto set = document->set({chord::path_key("taskId")}, numeric(task->value)); !set)
                    return std::unexpected(set.error());
                *created = *task;
                return std::nullopt;
            }));
    if (!committed) return std::unexpected(committed.error());
    return *created;
}

namespace {

/// Run the `beforeTool` hooks of one nested call: the first block wins and every
/// `arguments` decision replaces the call's arguments (frozen pi `ToolTask.call`).
support::ExpectedVoid before_tool_hooks(
        TaskRuntime& runtime, const NestedCall& call, Json& arguments, std::optional<std::string>& block) {
    Json hook_input = hook_call_json(call, arguments);
    // `arguments` and `block` are borrowed into the completion lambda: the
    // `await_result` below blocks this frame until the hook completes, so they
    // outlive every invocation of it (CODING_STANDARDS.md §7.5).
    auto hooked = await_result(runtime.each_hook("beforeTool",
            [input = std::move(hook_input), &arguments, &block, &runtime](
                    TaskHook& handler) -> support::AsyncResult<void> {
                if (block.has_value()) return support::AsyncResult<void>{support::ExpectedVoid{}};
                auto returned = handler(input, runtime);
                return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                        [returned = std::move(returned), &arguments, &block](
                                support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                            std::move(returned).start([&arguments, &block, done = std::move(done)](
                                                              support::Expected<Json> value) mutable noexcept {
                                if (!value) {
                                    done(std::unexpected(value.error()));
                                    return;
                                }
                                if (const Json* reason = member(*value, "block");
                                        reason != nullptr && reason->as_string() != nullptr) {
                                    block = *reason->as_string();
                                } else if (const Json* rewritten = member(*value, "arguments");
                                        rewritten != nullptr && rewritten->as_object() != nullptr) {
                                    arguments = *rewritten;
                                }
                                done(support::ExpectedVoid{});
                            });
                        }}};
            }));
    if (!hooked) return std::unexpected(hooked.error());
    return {};
}

/// Apply the `afterTool` hooks to one nested call's result: every replacement
/// becomes the next handler's result (frozen pi `ToolTask.finalResult`).
support::Expected<ai::ToolResultMessage> after_tool_hooks(TaskRuntime& runtime,
        const NestedCall& call,
        const Json& arguments,
        ai::ToolResultMessage message,
        std::optional<std::int64_t> duration_ms) {
    Object fields{{"output", content_json(message.content)}, {"isError", message.is_error}};
    if (message.details) fields.emplace("details", *Json::from_support(*message.details));
    Json final{std::move(fields)};
    Json hook_input{Object{{"call", hook_call_json(call, arguments)}, {"result", final}}};
    // `final_ptr` is borrowed into the completion lambda: the `await_result`
    // below blocks this frame until the hook completes (CODING_STANDARDS.md §7.5).
    auto* final_ptr = &final;
    auto hooked = await_result(runtime.each_hook("afterTool",
            [input = std::move(hook_input), final_ptr, &runtime](TaskHook& handler) -> support::AsyncResult<void> {
                auto returned = handler(input, runtime);
                return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                        [returned = std::move(returned), final_ptr](
                                support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                            std::move(returned).start([final_ptr, done = std::move(done)](
                                                              support::Expected<Json> value) mutable noexcept {
                                if (!value) {
                                    done(std::unexpected(value.error()));
                                    return;
                                }
                                if (value->as_object() != nullptr) *final_ptr = std::move(*value);
                                done(support::ExpectedVoid{});
                            });
                        }}};
            }));
    if (!hooked) return std::unexpected(hooked.error());
    ai::ToolResultMessage result{.tool_call_id = call.call.id, .tool_name = call.call.name};
    if (const Json* blocks = member(final, "output"); blocks != nullptr) {
        auto decoded = content_from_json(*blocks);
        if (!decoded) return std::unexpected(decoded.error());
        result.content = std::move(*decoded);
    }
    if (const Json* is_error = member(final, "isError"); is_error != nullptr && is_error->as_boolean() != nullptr)
        result.is_error = *is_error->as_boolean();
    if (const Json* details = member(final, "details")) result.details = details->to_support();
    result.duration_ms = duration_ms;
    return result;
}

/// Run the resolved tool, then settle its (hook-replaced) result.
support::ExpectedVoid run_tool(TaskRuntime& runtime,
        Session& session,
        const NestedCall& call,
        const std::shared_ptr<ToolRegistration>& tool,
        const Json& arguments) {
    ToolExecutionContext context{.runtime = &runtime, .session = &session, .call_id = call.call.id};
    for (const auto& candidate : runtime.callable_tools()) {
        if (!candidate) continue;
        context.tools.push_back(candidate->declaration);
        if (candidate->deferred) context.deferred_tools.push_back(candidate->declaration.name);
        if (candidate->replay_safe) context.replay_safe_tools.push_back(candidate->declaration.name);
    }
    // A nested call's own `executeTool` seam stays unwired in this slice: the
    // model-issued call excludes its own tool from a script's tool list, so a
    // script cannot reach a nested codemode call and admit grandchildren.
    const auto started = std::chrono::steady_clock::now();
    support::Expected<ai::ToolResultMessage> output =
            std::unexpected(validation("A nested call names no executable tool"));
    if (tool->execute_durable) {
        output = tool->execute_durable(arguments.to_support(), std::move(context));
    } else if (tool->execute) {
        auto blocks = await_result(tool->execute(arguments.to_support(), runtime.stop_token()));
        if (!blocks) {
            if (blocks.error().code == support::ErrorCode::Cancelled && runtime.stop_token().stop_requested())
                return std::unexpected(blocks.error());
            output = ai::ToolResultMessage{.content = {ai::text_content(blocks.error().message)}, .is_error = true};
        } else {
            output = ai::ToolResultMessage{.content = std::move(*blocks)};
        }
    }
    if (!output) return std::unexpected(output.error());
    const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    // The hooks see the call the script made; the intent's rewritten arguments
    // are what executed (frozen pi `finalResult` reads the original call).
    auto final = after_tool_hooks(runtime, call, arguments_of(call.call), std::move(*output), duration);
    if (!final) return std::unexpected(final.error());
    return settle(runtime,
            call,
            result_json(final->content, final->is_error, final->details, final->duration_ms, {}),
            "completed");
}

} // namespace

DocumentDefinition nested_call_document() {
    auto definition = define_document_family(DocumentDefinitionBuilder{"pi.tool.nested-call:v1"}.task(),
            [](const chord::Json&) { return chord::Json{chord::Json::Object{}}; });
    if (!definition) std::terminate(); // fixed valid built-in definition
    return std::move(*definition);
}

support::Expected<NestedCallResult> decode_nested_result(const chord::Json& result) {
    const Json* is_error = member(result, "isError");
    const Json* content = member(result, "content");
    if (is_error == nullptr || is_error->as_boolean() == nullptr || content == nullptr)
        return std::unexpected(validation("A nested call result is incomplete"));
    auto decoded = content_from_json(*content);
    if (!decoded) return std::unexpected(decoded.error());
    NestedCallResult nested{.is_error = *is_error->as_boolean(), .content = std::move(*decoded)};
    if (const Json* details = member(result, "details")) nested.details = details->to_support();
    if (const Json* duration = member(result, "durationMs"); duration != nullptr) {
        if (auto parsed = as_integer(*duration)) nested.duration_ms = *parsed;
    }
    if (const Json* diagnostics = member(result, "diagnostics"); diagnostics != nullptr) {
        const auto* listed = diagnostics->as_array();
        if (listed == nullptr) return std::unexpected(validation("A nested call's diagnostics must be an array"));
        for (const Json& diagnostic : *listed) {
            const Json* code = member(diagnostic, "code");
            const Json* message = member(diagnostic, "message");
            if (code == nullptr || code->as_string() == nullptr || message == nullptr ||
                    message->as_string() == nullptr)
                return std::unexpected(validation("A nested call diagnostic is incomplete"));
            nested.diagnostics.push_back(
                    NestedCallDiagnostic{.code = *code->as_string(), .message = *message->as_string()});
        }
    }
    return nested;
}

ai::ToolResultMessage nested_result_message(NestedCallResult result, std::string call_id, std::string name) {
    ai::ToolResultMessage message{.tool_call_id = std::move(call_id),
            .tool_name = std::move(name),
            .content = std::move(result.content),
            .details = std::move(result.details),
            .is_error = result.is_error,
            .duration_ms = result.duration_ms};
    // A harness-decided error carries no content of its own: its diagnostic text
    // is what the caller reports (frozen pi `outcomeOf`).
    if (message.content.empty() && !result.diagnostics.empty()) {
        std::string text;
        for (const auto& diagnostic : result.diagnostics) {
            if (!text.empty()) text += '\n';
            text += diagnostic.message;
        }
        message.content.push_back(ai::text_content(std::move(text)));
    }
    return message;
}

support::Expected<TaskDefinition> nested_tool_task(Session& session) {
    TaskDefinitionBuilder builder{std::string{kToolTaskKind}, 2};
    builder.initial([](const chord::Json&) { return chord::Json{Object{{"phase", "call"}}}; });
    builder.hook("beforeTool");
    builder.hook("afterTool");
    // The call phase: resolve, hook, record intent, execute (frozen pi `ToolTask.call`).
    builder.phase("call", [&session](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto call = parse_call(task.input);
        if (!call) return support::AsyncResult<void>{std::unexpected(call.error())};
        auto tool = resolve_tool(runtime, call->call.name);
        if (tool == nullptr)
            return support::AsyncResult<void>{settle_harness_error(
                    runtime, *call, "tool_unavailable", std::format("Tool {} is not available", call->call.name))};
        if (!call->call.arguments_valid)
            return support::AsyncResult<void>{
                    settle_harness_error(runtime, *call, "invalid_arguments", "Invalid tool arguments")};
        Json arguments = arguments_of(call->call);
        std::optional<std::string> block;
        auto hooked = before_tool_hooks(runtime, *call, arguments, block);
        if (!hooked) return support::AsyncResult<void>{std::unexpected(hooked.error())};
        if (block.has_value())
            return support::AsyncResult<void>{
                    settle_harness_error(runtime, *call, "blocked", std::format("Tool call blocked: {}", *block))};
        // The intent commits before the physical operation, so a recovery
        // knows whether the call may have run.
        auto intended = await_result(runtime.commit([&arguments, &tool](Transaction&, const TaskRecord&)
                                                            -> support::Expected<std::optional<NextTaskState>> {
            Object checkpoint{
                    {"phase", "execute"}, {"arguments", arguments}, {"replay", tool->replay_safe ? "safe" : "unsafe"}};
            return std::optional<NextTaskState>{NextTaskState::running(Json{std::move(checkpoint)})};
        }));
        if (!intended) return support::AsyncResult<void>{std::unexpected(intended.error())};
        return support::AsyncResult<void>{run_tool(runtime, session, *call, tool, arguments)};
    });
    // The execute phase is reached only by recovery after the intent (frozen pi `ToolTask.execute`).
    builder.phase("execute", [&session](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto call = parse_call(task.input);
        if (!call) return support::AsyncResult<void>{std::unexpected(call.error())};
        const Json* checkpoint = task.state.checkpoint.has_value() ? &*task.state.checkpoint : nullptr;
        const Json* arguments = checkpoint == nullptr ? nullptr : member(*checkpoint, "arguments");
        const Json* replay = checkpoint == nullptr ? nullptr : member(*checkpoint, "replay");
        auto tool = resolve_tool(runtime, call->call.name);
        const bool safe = arguments != nullptr && replay != nullptr && replay->as_string() != nullptr &&
                          *replay->as_string() == "safe" && tool != nullptr && tool->replay_safe;
        if (!safe)
            return support::AsyncResult<void>{settle_harness_error(runtime,
                    *call,
                    "interrupted",
                    std::format("Tool {} was interrupted and may have partially run", call->call.name))};
        return support::AsyncResult<void>{run_tool(runtime, session, *call, tool, *arguments)};
    });
    // The abort handler settles the result its caller waits on (frozen pi `ToolTask.abort`).
    builder.abort([](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto call = parse_call(task.input);
        if (!call) return support::AsyncResult<void>{std::unexpected(call.error())};
        return support::AsyncResult<void>{settle_harness_error(
                runtime, *call, "aborted", std::format("Tool {} was aborted", call->call.name), "aborted")};
    });
    return builder.build();
}

} // namespace cch::durable
