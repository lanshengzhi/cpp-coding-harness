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

/// One `pi.tool` task input: a model-issued call read from its assistant entry,
/// or a nested call whose arguments the input itself carries (frozen pi
/// `ToolTaskInput`).
struct ParsedInput {
    bool nested{false};
    NestedCall nested_call{};
    std::int64_t assistant{0};
    std::string call_id{};
};

support::Expected<ParsedInput> parse_input(const Json& input) {
    const Json* kind = member(input, "kind");
    if (kind == nullptr || kind->as_string() == nullptr)
        return std::unexpected(validation("A tool task needs a complete input"));
    if (*kind->as_string() == "nested") {
        auto nested = parse_call(input);
        if (!nested) return std::unexpected(nested.error());
        ParsedInput parsed{.nested = true, .nested_call = std::move(*nested)};
        return parsed;
    }
    if (*kind->as_string() != "model") return std::unexpected(validation("A tool task input kind is unknown"));
    const Json* assistant = member(input, "assistant");
    const Json* call_id = member(input, "callId");
    if (assistant == nullptr || call_id == nullptr || call_id->as_string() == nullptr)
        return std::unexpected(validation("A model tool task needs an assistant entry and call ID"));
    auto entry = as_integer(*assistant);
    if (!entry) return std::unexpected(validation("A model tool task needs an assistant entry ID"));
    ParsedInput parsed{.nested = false, .assistant = *entry, .call_id = *call_id->as_string()};
    return parsed;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

const support::JsonValue* support_field(const support::JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<support::JsonValue::object_t>();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

/// The model-issued call from its committed assistant entry (frozen pi
/// `ToolTask.readCall` for a `model` input).
support::Expected<ai::ToolCallContent> read_model_call(Session& session, const ParsedInput& input) {
    support::JsonValue data;
    auto loaded =
            await_result(session.commit([&data, assistant = input.assistant](Transaction& tx) -> support::ExpectedVoid {
                auto entry = tx.entry(EntryId{.value = assistant});
                if (!entry) return std::unexpected(entry.error());
                if (!entry->has_value())
                    return std::unexpected(validation("A tool task names a missing assistant entry"));
                data = (*entry)->data.to_support();
                return {};
            }));
    if (!loaded) return std::unexpected(loaded.error());
    const auto* model = support_field(data, "model");
    if (model == nullptr || !model->holds<support::JsonValue::array_t>() || model->get_array().empty())
        return std::unexpected(validation("A tool task names an assistant entry without a message"));
    const auto& message = model->get_array().front();
    const auto* content = support_field(message, "content");
    if (content == nullptr || !content->holds<support::JsonValue::array_t>())
        return std::unexpected(validation("An assistant entry has no content"));
    for (const auto& block : content->get_array()) {
        const auto* type = support_field(block, "type");
        const auto* id = support_field(block, "id");
        if (type == nullptr || !type->holds<std::string>() || type->get_string() != "toolCall" || id == nullptr ||
                !id->holds<std::string>() || id->get_string() != input.call_id)
            continue;
        const auto* name = support_field(block, "name");
        if (name == nullptr || !name->holds<std::string>())
            return std::unexpected(validation("A tool call block has no name"));
        ai::ToolCallContent call{.id = input.call_id, .name = name->get_string()};
        if (const auto* arguments = support_field(block, "arguments");
                arguments != nullptr && arguments->holds<support::JsonValue::object_t>())
            call.arguments = *arguments;
        else if (arguments != nullptr && !arguments->holds<std::nullptr_t>())
            call.arguments_valid = false;
        if (const auto* signature = support_field(block, "thoughtSignature");
                signature != nullptr && signature->holds<std::string>())
            call.thought_signature = signature->get_string();
        return call;
    }
    return std::unexpected(validation("An assistant entry has no such tool call"));
}

/// The call a `pi.tool` input names: carried by a nested input, or read from
/// its assistant entry for a model-issued input.
support::Expected<ai::ToolCallContent> input_call(Session& session, const ParsedInput& input) {
    if (input.nested) return input.nested_call.call;
    return read_model_call(session, input);
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

/// What programs get of a tool's output when it declares no schema (frozen
/// pi `outputValue`): one text item as its string, one image as itself,
/// nothing as `""`, and anything else as the content list.
Json output_value(const std::vector<ai::Content>& content) {
    if (content.empty()) return Json{std::string{}};
    if (content.size() > 1) return content_json(content);
    const auto& only = content.front();
    if (const auto* text = std::get_if<ai::TextContent>(&only)) return Json{text->text};
    return content_block_json(only);
}

/// The stored result of one nested call (frozen pi `NestedToolExecutionResult`).
/// `taskId` and `structuredOutput` are the frozen members a pi run reads when it
/// continues this call; the harness keeps no separate `content` member.
Json result_json(std::int64_t task_id,
        const std::vector<ai::Content>& content,
        bool is_error,
        const std::optional<support::JsonValue>& details,
        std::optional<std::int64_t> duration_ms,
        const std::vector<NestedCallDiagnostic>& diagnostics) {
    Object value{{"taskId", numeric(task_id)}, {"structuredOutput", output_value(content)}, {"isError", is_error}};
    if (details) value.emplace("details", *Json::from_support(*details));
    if (duration_ms) value.emplace("durationMs", numeric(*duration_ms));
    {
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

/// One model-issued tool result entry's message (frozen pi `appendToolResult`).
Json tool_result_json(const ai::ToolCallContent& call, const ai::ToolResultMessage& message) {
    Object value{{"role", "toolResult"},
            {"toolCallId", call.id},
            {"toolName", call.name},
            {"content", content_json(message.content)},
            {"isError", message.is_error},
            {"timestamp", numeric(now_ms())}};
    if (message.details) value.emplace("details", *Json::from_support(*message.details));
    if (message.duration_ms) value.emplace("durationMs", numeric(*message.duration_ms));
    return Json{std::move(value)};
}

Json diagnostic_list(const std::vector<NestedCallDiagnostic>& diagnostics) {
    Array listed;
    for (const auto& diagnostic : diagnostics)
        listed.push_back(diagnostic_json(diagnostic));
    return Json{std::move(listed)};
}

/// Settle a model-issued call: append its `pi.tool-result` entry and end with
/// the frozen `{kind: "model", entryId}` receipt (frozen pi `ToolTask.settle`).
support::ExpectedVoid settle_model(TaskRuntime& runtime,
        const ai::ToolCallContent& call,
        ai::ToolResultMessage message,
        std::vector<NestedCallDiagnostic> diagnostics,
        std::string status) {
    Json entry_message = tool_result_json(call, message);
    Json payload{Object{{"model", Json{Array{entry_message}}},
            {"data", Json{Object{{"diagnostics", diagnostic_list(diagnostics)}}}}}};
    const auto conversation = runtime.conversation_id();
    const auto task_id = runtime.task_id();
    return await_result(runtime.commit_result(
            [payload = std::move(payload),
                    message = std::move(entry_message),
                    status = std::move(status),
                    conversation,
                    task_id](Transaction& tx, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                auto entry = tx.append_entry(conversation, "pi.tool-result", payload, std::nullopt, {message}, task_id);
                if (!entry) return std::unexpected(entry.error());
                return std::optional<NextTaskState>{NextTaskState::terminal(TaskOutcome{.status = status,
                        .result = Json{Object{{"kind", "model"}, {"entryId", numeric(entry->id.value)}}}})};
            }));
}

/// Settle an error result the harness itself decided: its diagnostic is what the
/// caller reports, as a model-visible call would show it (frozen pi `harnessError`).
support::ExpectedVoid settle_harness_error(TaskRuntime& runtime,
        const ParsedInput& input,
        const ai::ToolCallContent& call,
        std::string code,
        std::string message,
        std::string status = "completed") {
    std::vector<NestedCallDiagnostic> diagnostics{
            NestedCallDiagnostic{.code = std::move(code), .message = std::move(message)}};
    if (input.nested)
        return settle(runtime,
                input.nested_call,
                result_json(runtime.task_id().value, {}, true, std::nullopt, std::nullopt, diagnostics),
                std::move(status));
    ai::ToolResultMessage error{.content = harness_error_content(diagnostics.front().message), .is_error = true};
    return settle_model(runtime, call, std::move(error), std::move(diagnostics), std::move(status));
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
        TaskRuntime& runtime, Json hook_input, Json& arguments, std::optional<std::string>& block) {
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
        const ai::ToolCallContent& call,
        Json call_view,
        ai::ToolResultMessage message,
        std::optional<std::int64_t> duration_ms) {
    Object fields{{"output", content_json(message.content)}, {"isError", message.is_error}};
    if (message.details) fields.emplace("details", *Json::from_support(*message.details));
    Json final{std::move(fields)};
    Json hook_input{Object{{"call", std::move(call_view)}, {"result", final}}};
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
    ai::ToolResultMessage result{.tool_call_id = call.id, .tool_name = call.name};
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
        const ParsedInput& input,
        const ai::ToolCallContent& call,
        const std::shared_ptr<ToolRegistration>& tool,
        const Json& arguments) {
    ToolExecutionContext context{.runtime = &runtime, .session = &session, .call_id = call.id};
    for (const auto& candidate : runtime.callable_tools()) {
        if (!candidate || candidate->declaration.name == call.name) continue;
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
    Json call_view =
            input.nested ? hook_call_json(input.nested_call, arguments_of(call)) : call_json(call, arguments_of(call));
    auto final = after_tool_hooks(runtime, call, std::move(call_view), std::move(*output), duration);
    if (!final) return std::unexpected(final.error());
    if (input.nested)
        return settle(runtime,
                input.nested_call,
                result_json(runtime.task_id().value,
                        final->content,
                        final->is_error,
                        final->details,
                        final->duration_ms,
                        {}),
                "completed");
    return settle_model(runtime, call, std::move(*final), {}, "completed");
}

} // namespace

DocumentDefinition nested_call_document() {
    auto definition = define_document_family(DocumentDefinitionBuilder{"pi.tool.nested-call"}.task(),
            [](const chord::Json&) { return chord::Json{chord::Json::Object{}}; });
    if (!definition) std::terminate(); // fixed valid built-in definition
    return std::move(*definition);
}

/// The model content a stored `structuredOutput` represents. A value a
/// schema-less tool could not produce (an object other than an image block) is
/// rendered as JSON text so a caller still sees it.
support::Expected<std::vector<ai::Content>> structured_content(const Json& value) {
    if (const auto* text = value.as_string()) return std::vector<ai::Content>{ai::text_content(*text)};
    if (const auto* blocks = value.as_array(); blocks != nullptr) return content_from_json(value);
    if (const Json* type = member(value, "type");
            type != nullptr && type->as_string() != nullptr && *type->as_string() == "image") {
        return content_from_json(Json{Array{value}});
    }
    auto text = support::write_json(value.to_support());
    if (!text) return std::unexpected(text.error());
    return std::vector<ai::Content>{ai::text_content(std::move(*text))};
}

support::Expected<NestedCallResult> decode_nested_result(const chord::Json& result) {
    const Json* is_error = member(result, "isError");
    if (is_error == nullptr || is_error->as_boolean() == nullptr)
        return std::unexpected(validation("A nested call result is incomplete"));
    NestedCallResult nested{.is_error = *is_error->as_boolean()};
    if (const Json* structured = member(result, "structuredOutput"); structured != nullptr) {
        nested.structured_output = structured->to_support();
        auto decoded = structured_content(*structured);
        if (!decoded) return std::unexpected(decoded.error());
        nested.content = std::move(*decoded);
    } else if (const Json* content = member(result, "content"); content != nullptr) {
        // A result written before the frozen member existed carries content
        // only; derive the value programs see from what it holds.
        auto decoded = content_from_json(*content);
        if (!decoded) return std::unexpected(decoded.error());
        nested.structured_output = output_value(*decoded).to_support();
        nested.content = std::move(*decoded);
    } else {
        return std::unexpected(validation("A nested call result has no value"));
    }
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
        auto input = parse_input(task.input);
        if (!input) return support::AsyncResult<void>{std::unexpected(input.error())};
        auto loaded = input_call(session, *input);
        if (!loaded) return support::AsyncResult<void>{std::unexpected(loaded.error())};
        const ai::ToolCallContent call = std::move(*loaded);
        auto tool = resolve_tool(runtime, call.name);
        if (tool == nullptr)
            return support::AsyncResult<void>{settle_harness_error(
                    runtime, *input, call, "tool_unavailable", std::format("Tool {} is not available", call.name))};
        if (!call.arguments_valid)
            return support::AsyncResult<void>{
                    settle_harness_error(runtime, *input, call, "invalid_arguments", "Invalid tool arguments")};
        Json arguments = arguments_of(call);
        Json hook_call = input->nested ? hook_call_json(input->nested_call, arguments) : call_json(call, arguments);
        std::optional<std::string> block;
        auto hooked = before_tool_hooks(runtime, std::move(hook_call), arguments, block);
        if (!hooked) return support::AsyncResult<void>{std::unexpected(hooked.error())};
        if (block.has_value())
            return support::AsyncResult<void>{settle_harness_error(
                    runtime, *input, call, "blocked", std::format("Tool call blocked: {}", *block))};
        // The intent commits before the physical operation, so a recovery
        // knows whether the call may have run.
        auto intended = await_result(runtime.commit([&arguments, &tool](Transaction&, const TaskRecord&)
                                                            -> support::Expected<std::optional<NextTaskState>> {
            Object checkpoint{
                    {"phase", "execute"}, {"arguments", arguments}, {"replay", tool->replay_safe ? "safe" : "unsafe"}};
            return std::optional<NextTaskState>{NextTaskState::running(Json{std::move(checkpoint)})};
        }));
        if (!intended) return support::AsyncResult<void>{std::unexpected(intended.error())};
        return support::AsyncResult<void>{run_tool(runtime, session, *input, call, tool, arguments)};
    });
    // The execute phase is reached only by recovery after the intent (frozen pi `ToolTask.execute`).
    builder.phase("execute", [&session](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto input = parse_input(task.input);
        if (!input) return support::AsyncResult<void>{std::unexpected(input.error())};
        auto loaded = input_call(session, *input);
        if (!loaded) return support::AsyncResult<void>{std::unexpected(loaded.error())};
        const ai::ToolCallContent call = std::move(*loaded);
        const Json* checkpoint = task.state.checkpoint.has_value() ? &*task.state.checkpoint : nullptr;
        const Json* arguments = checkpoint == nullptr ? nullptr : member(*checkpoint, "arguments");
        const Json* replay = checkpoint == nullptr ? nullptr : member(*checkpoint, "replay");
        auto tool = resolve_tool(runtime, call.name);
        const bool safe = arguments != nullptr && replay != nullptr && replay->as_string() != nullptr &&
                          *replay->as_string() == "safe" && tool != nullptr && tool->replay_safe;
        if (!safe)
            return support::AsyncResult<void>{settle_harness_error(runtime,
                    *input,
                    call,
                    "interrupted",
                    std::format("Tool {} was interrupted and may have partially run", call.name),
                    "failed")};
        return support::AsyncResult<void>{run_tool(runtime, session, *input, call, tool, *arguments)};
    });
    // The abort handler settles the result its caller waits on (frozen pi `ToolTask.abort`).
    builder.abort([&session](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto input = parse_input(task.input);
        if (!input) return support::AsyncResult<void>{std::unexpected(input.error())};
        auto loaded = input_call(session, *input);
        if (!loaded) return support::AsyncResult<void>{std::unexpected(loaded.error())};
        const ai::ToolCallContent call = std::move(*loaded);
        return support::AsyncResult<void>{settle_harness_error(
                runtime, *input, call, "aborted", std::format("Tool {} was aborted", call.name), "aborted")};
    });
    return builder.build();
}

} // namespace cch::durable
