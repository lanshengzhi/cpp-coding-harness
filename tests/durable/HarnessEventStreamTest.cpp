#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Harness.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/Gate.hpp"
#include "support/Json.hpp"
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

constexpr std::string_view kEventArtifact = "fixtures/pi-v1.3.0/audit-1173/event-stream.json";

using Json = chord::Json;
using Object = Json::Object;
using Array = Json::Array;

Json number_json(const std::int64_t value) { return Json(static_cast<double>(value)); }

const Json* member_of(const Json& value, const std::string_view name) {
    const auto* object = value.as_object();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{name});
    return found == object->end() ? nullptr : &found->second;
}

std::optional<std::string> string_of(const Json& value, const std::string_view name) {
    const Json* member = member_of(value, name);
    if (member == nullptr || member->as_string() == nullptr) return std::nullopt;
    return *member->as_string();
}

std::optional<double> number_of(const Json& value, const std::string_view name) {
    const Json* member = member_of(value, name);
    if (member == nullptr || member->as_number() == nullptr) return std::nullopt;
    return *member->as_number();
}

/// Batches one stream delivered; the listener runs on the Session publication
/// line, so the test thread reads under this lock.
class EventCollector {
public:
    void push(const AgentEventBatch& batch) {
        std::lock_guard lock(mutex_);
        std::vector<Json> events;
        events.reserve(batch.size());
        for (const AgentEvent& event : batch)
            events.push_back(event.value);
        batches_.push_back(std::move(events));
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return batches_.size();
    }

    [[nodiscard]] std::vector<std::vector<Json>> batches() const {
        std::lock_guard lock(mutex_);
        return batches_;
    }

    [[nodiscard]] std::vector<Json> flattened() const {
        std::lock_guard lock(mutex_);
        std::vector<Json> events;
        for (const auto& batch : batches_) {
            for (const Json& event : batch)
                events.push_back(event);
        }
        return events;
    }

    [[nodiscard]] bool wait_for(
            const std::size_t count, const std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] { return batches_.size() >= count; });
    }

    [[nodiscard]] bool wait_until(const std::function<bool(const std::vector<std::vector<Json>>&)>& predicate,
            const std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, &predicate] { return predicate(batches_); });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::vector<Json>> batches_{};
};

/// Wait until the stream delivered one event of `type`. The listener runs on the
/// commit-publication line, and the submission waiter may resolve before the
/// final commit's observation dispatch, so a settled harness is not yet a
/// delivered final batch.
[[nodiscard]] bool wait_for_event_count(
        const std::shared_ptr<EventCollector>& collector, const std::string_view type, const std::size_t count) {
    return collector->wait_until([type, count](const std::vector<std::vector<Json>>& batches) {
        std::size_t seen = 0;
        for (const auto& batch : batches) {
            for (const Json& event : batch) {
                if (string_of(event, "type") == std::optional<std::string>{std::string{type}}) ++seen;
            }
        }
        return seen >= count;
    });
}

[[nodiscard]] bool wait_for_event(const std::shared_ptr<EventCollector>& collector, const std::string_view type) {
    return collector->wait_until([type](const std::vector<std::vector<Json>>& batches) {
        for (const auto& batch : batches) {
            for (const Json& event : batch) {
                if (string_of(event, "type") == std::optional<std::string>{std::string{type}}) return true;
            }
        }
        return false;
    });
}

AgentEventListener collector_listener(const std::shared_ptr<EventCollector>& collector) {
    return [collector](const AgentEventBatch& batch) -> support::ExpectedVoid {
        collector->push(batch);
        return {};
    };
}

/// The stable event fields of one event, reduced the way the frozen capture
/// reduced them (`fixtures/pi-v1.3.0/audit-1173/support.mts#summarize`).
[[nodiscard]] Json summarize(const Json& event) {
    const auto type = string_of(event, "type").value_or("");
    Object summary{{"type", Json(type)}};
    for (const char* key : {"toolCallId", "toolName", "parentToolCallId"}) {
        if (const auto value = string_of(event, key); value.has_value()) summary.emplace(key, Json(*value));
    }
    if (member_of(event, "taskId") != nullptr) summary.emplace("hasTaskId", Json(true));
    if (member_of(event, "parentTaskId") != nullptr) summary.emplace("hasParentTaskId", Json(true));
    for (const char* key : {"args", "output", "details", "diagnostics"}) {
        if (const Json* value = member_of(event, key); value != nullptr) summary.emplace(key, *value);
    }
    if (const Json* entry = member_of(event, "entry"); entry != nullptr) {
        summary.emplace("entryKind", Json(string_of(*entry, "kind").value_or("")));
    }
    if (const Json* result = member_of(event, "result"); result != nullptr && result->as_object() != nullptr) {
        Array keys;
        for (const auto& [key, value] : *result->as_object()) {
            (void)value;
            keys.emplace_back(key);
        }
        std::ranges::sort(
                keys, [](const Json& left, const Json& right) { return *left.as_string() < *right.as_string(); });
        summary.emplace("resultKeys", Json{std::move(keys)});
    }
    if (const Json* message = member_of(event, "message"); message != nullptr) {
        const std::string role = string_of(*message, "role").value_or("");
        summary.emplace("role", Json(role));
        if (role == "toolResult") {
            const Json* content = member_of(*message, "content");
            const auto size = content != nullptr && content->as_array() != nullptr ? content->as_array()->size() : 0;
            summary.emplace("contentLength", number_json(static_cast<std::int64_t>(size)));
        } else if (role == "system") {
            Array sections;
            if (const Json* listed = member_of(*message, "sections");
                    listed != nullptr && listed->as_object() != nullptr) {
                for (const auto& [key, value] : *listed->as_object()) {
                    (void)value;
                    sections.emplace_back(key);
                }
            }
            summary.emplace("sections", Json{std::move(sections)});
        } else {
            std::string text;
            if (const Json* content = member_of(*message, "content"); content != nullptr) {
                if (content->as_string() != nullptr) {
                    text = *content->as_string();
                } else if (content->as_array() != nullptr) {
                    for (const Json& block : *content->as_array()) {
                        if (string_of(block, "type") == std::optional<std::string>{"text"}) {
                            text += string_of(block, "text").value_or("");
                        }
                    }
                }
            }
            summary.emplace("text", Json(text));
        }
    }
    if (member_of(event, "inputs") != nullptr) {
        const Json* inputs = member_of(event, "inputs");
        const auto size = inputs->as_array() != nullptr ? inputs->as_array()->size() : 0;
        summary.emplace("inputsCount", number_json(static_cast<std::int64_t>(size)));
    }
    if (const Json* changes = member_of(event, "changes"); changes != nullptr && changes->as_array() != nullptr) {
        Array types;
        for (const Json& change : *changes->as_array()) {
            types.emplace_back(string_of(change, "type").value_or(""));
        }
        summary.emplace("changeTypes", Json{std::move(types)});
    }
    if (member_of(event, "usage") != nullptr) summary.emplace("hasUsage", Json(true));
    if (const Json* record = member_of(event, "record"); record != nullptr) {
        summary.emplace("submission",
                Json{Object{{"type", Json(string_of(*record, "type").value_or(""))},
                        {"status", Json(string_of(*record, "status").value_or(""))}}});
    }
    if (const Json* agent = member_of(event, "agent"); agent != nullptr && agent->as_object() != nullptr) {
        Array keys;
        for (const auto& [key, value] : *agent->as_object()) {
            (void)value;
            keys.emplace_back(key);
        }
        std::ranges::sort(
                keys, [](const Json& left, const Json& right) { return *left.as_string() < *right.as_string(); });
        summary.emplace("agentKeys", Json{std::move(keys)});
    }
    if (const Json* items = member_of(event, "items"); items != nullptr && items->as_array() != nullptr) {
        summary.emplace("items", number_json(static_cast<std::int64_t>(items->as_array()->size())));
    }
    if (type == "task_failed") {
        summary.emplace("kind", Json(string_of(event, "kind").value_or("")));
        summary.emplace("message", Json(string_of(event, "message").value_or("")));
    }
    for (const char* key : {"error", "errorMessage"}) {
        if (const auto value = string_of(event, key); value.has_value()) summary.emplace(key, Json(*value));
    }
    for (const char* key : {"attempt", "blocking"}) {
        if (const auto value = number_of(event, key); value.has_value()) {
            summary.emplace(key, number_json(static_cast<std::int64_t>(*value)));
        }
    }
    if (const auto reason = string_of(event, "reason"); reason.has_value()) summary.emplace("reason", Json(*reason));
    return Json{std::move(summary)};
}

[[nodiscard]] Json snapshot_summary(const Json& snapshot) {
    const Json* entries = member_of(snapshot, "entries");
    const Json* run = member_of(snapshot, "run");
    const Json* generation = member_of(snapshot, "generation");
    Object summary{{"type", Json(string_of(snapshot, "type").value_or(""))},
            {"entries",
                    number_json(entries != nullptr && entries->as_array() != nullptr
                                        ? static_cast<std::int64_t>(entries->as_array()->size())
                                        : 0)},
            {"runInputs",
                    number_json(run != nullptr && member_of(*run, "inputs") != nullptr &&
                                                member_of(*run, "inputs")->as_array() != nullptr
                                        ? static_cast<std::int64_t>(member_of(*run, "inputs")->as_array()->size())
                                        : 0)}};
    if (generation == nullptr) {
        summary.emplace("generation", Json{});
    } else {
        summary.emplace("generation", Json{Object{{"hasMessage", Json(member_of(*generation, "message") != nullptr)}}});
    }
    Array tools;
    if (const Json* listed = member_of(snapshot, "tools"); listed != nullptr && listed->as_array() != nullptr) {
        for (const Json& slot : *listed->as_array()) {
            tools.emplace_back(Json{Object{{"callId", Json(string_of(slot, "callId").value_or(""))},
                    {"name", Json(string_of(slot, "name").value_or(""))},
                    {"status", Json(string_of(slot, "status").value_or(""))}}});
        }
    }
    summary.emplace("tools", Json{std::move(tools)});
    const auto array_size = [&snapshot](const char* key) {
        const Json* listed = member_of(snapshot, key);
        return number_json(listed != nullptr && listed->as_array() != nullptr
                                   ? static_cast<std::int64_t>(listed->as_array()->size())
                                   : 0);
    };
    summary.emplace("nestedTools", array_size("nestedTools"));
    summary.emplace("compactions", array_size("compactions"));
    summary.emplace("inbox", array_size("inbox"));
    const auto sorted_keys = [](const Json* object) {
        Array keys;
        if (object != nullptr && object->as_object() != nullptr) {
            for (const auto& [key, value] : *object->as_object()) {
                (void)value;
                keys.emplace_back(key);
            }
        }
        std::ranges::sort(
                keys, [](const Json& left, const Json& right) { return *left.as_string() < *right.as_string(); });
        return Json{std::move(keys)};
    };
    summary.emplace("agentKeys", sorted_keys(member_of(snapshot, "agent")));
    summary.emplace("usageKeys", sorted_keys(member_of(snapshot, "usage")));
    return Json{std::move(summary)};
}

[[nodiscard]] Json batch_types(const std::vector<std::vector<Json>>& batches) {
    Array result;
    for (const auto& batch : batches) {
        Array types;
        for (const Json& event : batch)
            types.emplace_back(string_of(event, "type").value_or(""));
        result.emplace_back(Json{std::move(types)});
    }
    return Json{std::move(result)};
}

/// Fold consecutive batches that hold only `tool_execution_update` events into
/// one, matching the frozen capture where progress commits are paced and
/// coalesced (the C++ writer commits each report).
[[nodiscard]] Json collapse_batch_types(const Json& batches) {
    Array collapsed;
    bool in_update = false;
    if (const Array* listed = batches.as_array(); listed != nullptr) {
        for (const Json& batch : *listed) {
            const Array* types = batch.as_array();
            const bool only_updates =
                    types != nullptr && !types->empty() && std::ranges::all_of(*types, [](const Json& type) {
                        return type.as_string() != nullptr && *type.as_string() == "tool_execution_update";
                    });
            if (only_updates) {
                if (!in_update) collapsed.emplace_back(Json{Array{Json(std::string{"tool_execution_update"})}});
                in_update = true;
                continue;
            }
            in_update = false;
            collapsed.push_back(batch);
        }
    }
    return Json{std::move(collapsed)};
}

/// Fold consecutive `tool_execution_update` events the way the frozen capture
/// does: the accumulated running output and the last details. Frozen progress
/// commits are paced; the C++ writer commits each report.
[[nodiscard]] Json collapse_updates(const std::vector<Json>& events) {
    Array collapsed;
    struct Run {
        std::string call_id{};
        std::string name{};
        bool has_task{false};
        std::string text{};
        std::optional<Json> details{};
    };
    std::optional<Run> run;
    const auto flush = [&collapsed, &run] {
        if (!run.has_value()) return;
        Object value{{"type", Json(std::string{"tool_execution_update"})},
                {"toolCallId", Json(run->call_id)},
                {"toolName", Json(run->name)},
                {"hasTaskId", Json(run->has_task)},
                {"outputText", Json(run->text)}};
        if (run->details.has_value()) value.emplace("details", *run->details);
        collapsed.emplace_back(Json{std::move(value)});
        run.reset();
    };
    for (const Json& event : events) {
        if (string_of(event, "type") != std::optional<std::string>{"tool_execution_update"}) {
            flush();
            collapsed.push_back(summarize(event));
            continue;
        }
        const std::string call_id = string_of(event, "toolCallId").value_or("");
        if (!run.has_value() || run->call_id != call_id) {
            flush();
            run = Run{.call_id = call_id,
                    .name = string_of(event, "toolName").value_or(""),
                    .has_task = member_of(event, "taskId") != nullptr};
        }
        if (const Json* output = member_of(event, "output"); output != nullptr) {
            if (const Json* set = member_of(*output, "set"); set != nullptr && set->as_string() != nullptr) {
                run->text = *set->as_string();
            } else {
                if (const auto trim = number_of(*output, "trimStart"); trim.has_value()) {
                    run->text.erase(0, static_cast<std::size_t>(*trim));
                }
                if (const auto append = string_of(*output, "append"); append.has_value()) run->text += *append;
            }
        }
        if (const Json* details = member_of(event, "details"); details != nullptr) run->details = *details;
    }
    flush();
    return Json{std::move(collapsed)};
}

[[nodiscard]] Json settled_summary(const SettledSubmission& settled) {
    if (settled.status == "done") return Json{Object{{"status", Json(std::string{"done"})}}};
    Object value{{"status", Json(settled.status)}};
    if (settled.reason.has_value()) value.emplace("reason", Json(*settled.reason));
    return Json{std::move(value)};
}

[[nodiscard]] Json entry_kinds(Storage& storage, const ConversationId conversation) {
    auto entries = storage.scan_entries(conversation, SIZE_MAX);
    REQUIRE(entries.has_value());
    std::ranges::reverse(*entries);
    Array kinds;
    for (const EntryRecord& entry : *entries)
        kinds.emplace_back(entry.kind);
    return Json{std::move(kinds)};
}

[[nodiscard]] Json task_states(const HarnessInspection& inspection) {
    Array listed;
    for (const TaskInspection& task : inspection.tasks) {
        listed.emplace_back(Json{Object{{"kind", Json(task.record.kind)}, {"state", Json(task.state.kind)}}});
    }
    return Json{std::move(listed)};
}

void check_value(const Json& actual, const char* name, const FrozenScenario& frozen) {
    const support::JsonValue* expected = frozen.member(name);
    REQUIRE(expected != nullptr);
    auto actual_text = support::canonical_json(actual.to_support());
    auto expected_text = support::canonical_json(*expected);
    INFO("frozen field " << name << "\n  actual:   " << (actual_text ? *actual_text : std::string{"<unwritable>"})
                         << "\n  expected: " << (expected_text ? *expected_text : std::string{"<unwritable>"}));
    CHECK((actual_text.has_value() && expected_text.has_value() && *actual_text == *expected_text));
}

// ── Scripted model and tools ────────────────────────────────────────────────

struct ScriptedReply {
    ai::AssistantMessage message{};
    std::optional<ai::InferenceFailure> failure{};
};

[[nodiscard]] ai::AssistantMessage answer_message(std::string text) {
    auto message = ai::assistant_text_message(std::move(text));
    message.provider = "test";
    message.model = "model";
    message.usage.input = 3;
    return message;
}

[[nodiscard]] ai::AssistantMessage tool_call_message(
        std::string name, support::JsonValue arguments, std::string id = "c1") {
    auto message = ai::assistant_text_message("");
    message.provider = "test";
    message.model = "model";
    message.usage.input = 3;
    message.content = {
            ai::ToolCallContent{.id = std::move(id), .name = std::move(name), .arguments = std::move(arguments)}};
    message.stop_reason = ai::AssistantStopReason::ToolUse;
    return message;
}

[[nodiscard]] ai::ModelStreamFactory scripted_stream(std::vector<ScriptedReply> replies) {
    return [replies = std::move(replies), index = std::size_t{0}](
                   ai::Model, ai::AiContext, ai::SimpleStreamOptions) mutable {
        ScriptedReply reply = replies[std::min(index, replies.size() - 1)];
        ++index;
        return ai::ModelStream{ai::ModelStreamProducer{
                [reply = std::move(reply)](ai::AssistantEventSink sink, ai::ModelStreamCompletion done) noexcept {
                    const auto reason = reply.message.stop_reason;
                    support::ExpectedVoid delivered{};
                    if (reply.failure.has_value()) {
                        delivered = sink(ai::AssistantErrorEvent{
                                .reason = reason, .error = reply.message, .inference_failure = *reply.failure});
                    } else {
                        delivered = sink(ai::AssistantDoneEvent{.reason = reason, .message = reply.message});
                    }
                    if (!delivered) {
                        done(std::unexpected(delivered.error()));
                        return;
                    }
                    done(std::move(reply.message));
                }}};
    };
}

[[nodiscard]] ai::Tool print_declaration() {
    return ai::Tool{.name = "print",
            .description = "Prints",
            .parameters = support::JsonValue{support::JsonValue::object_t{{"type", "object"},
                    {"properties",
                            support::JsonValue{support::JsonValue::object_t{
                                    {"n", support::JsonValue{support::JsonValue::object_t{{"type", "number"}}}}}}}}}};
}

[[nodiscard]] ToolRegistration print_tool() {
    return ToolRegistration{.declaration = print_declaration(),
            .execute_durable = [](support::JsonValue, ToolExecutionContext context) {
                context.report_output("one\n");
                context.report_details(Json{Object{{"step", number_json(1)}}});
                context.report_output("two\n");
                context.report_details(Json{Object{{"step", number_json(2)}}});
                return support::Expected<ai::ToolResultMessage>{ai::ToolResultMessage{}};
            }};
}

[[nodiscard]] ToolRegistration gated_print_tool(const std::shared_ptr<Gate>& gate, const bool with_details = true) {
    return ToolRegistration{.declaration = print_declaration(),
            .execute_durable = [gate, with_details](support::JsonValue, ToolExecutionContext context) {
                context.report_output("one\n");
                if (with_details) context.report_details(Json{Object{{"step", number_json(1)}}});
                if (!gate->wait(context.runtime->stop_token())) {
                    return support::Expected<ai::ToolResultMessage>{
                            std::unexpected(support::make_error(support::ErrorCode::Cancelled, "gate aborted"))};
                }
                context.report_output("two\n");
                return support::Expected<ai::ToolResultMessage>{ai::ToolResultMessage{}};
            }};
}

[[nodiscard]] ToolRegistration throwing_tool() {
    return ToolRegistration{.declaration = print_declaration(),
            .execute = [](support::JsonValue, std::stop_token) -> support::AsyncResult<std::vector<ai::Content>> {
                return support::AsyncResult<std::vector<ai::Content>>{
                        std::unexpected(support::make_error(support::ErrorCode::Tool, "boom"))};
            }};
}

[[nodiscard]] SubmissionDraft text_input(std::string text) {
    return SubmissionDraft{.body = InputSubmissionDraft{.content = std::move(text)}};
}

[[nodiscard]] RootOptions test_root_options() {
    return RootOptions{.model = ai::Model{.id = "model", .provider = "test"}};
}

struct Opened {
    std::unique_ptr<Harness> harness{};
    ConversationId root{};
    std::unique_ptr<Storage> owned_storage{};
};

[[nodiscard]] Opened open_harness(Storage& storage, Registry& registry, ai::ModelStreamFactory stream) {
    HarnessOptions options{.registry = &registry};
    options.stream = std::move(stream);
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(test_root_options()));
    REQUIRE(root.has_value());
    return Opened{.harness = std::move(harness), .root = root.value()};
}

[[nodiscard]] AgentEventStream attach_events(Harness& harness, const ConversationId root) {
    auto opened = consume(harness.watch_events(root));
    REQUIRE(opened.has_value());
    return std::move(opened.value());
}

} // namespace

TEST_CASE("an agent event stream reports a model/tool round in committed order",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(kEventArtifact,
            "a model/tool round reports message, tool, usage and settlement lifecycle in committed order");
    REQUIRE(frozen.has_value());
    Registry registry;
    Extension extensions{.name = "event-tools"};
    extensions.tools.push_back(print_tool());
    REQUIRE(registry.install(std::move(extensions)).has_value());
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({
                    ScriptedReply{.message = tool_call_message(
                                          "print", support::JsonValue{support::JsonValue::object_t{{"n", 1.0}}}, "c1")},
                    ScriptedReply{.message = answer_message("done")},
            }));
    auto stream = attach_events(*opened.harness, opened.root);
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    const Json attached = snapshot_summary(stream.snapshot());

    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    const bool delivered_run_end = wait_for_event(collector, "run_end");
    const Json kinds = entry_kinds(storage, opened.root);
    const auto end = stream.stop();
    const auto closed = consume(opened.harness->close());

    // Release-then-assert (CODING_STANDARDS.md §11.10): the harness is closed
    // before any discriminating assertion can abort this frame.
    REQUIRE(closed.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered_run_end);
    check_value(attached, "snapshot", *frozen);
    const auto events = collector->flattened();
    check_value(collapse_updates(events), "collapsedEvents", *frozen);
    Object counts;
    const auto type_count = [&events](const std::string_view type) {
        return static_cast<std::int64_t>(std::ranges::count_if(events, [type](const Json& event) {
            return string_of(event, "type") == std::optional<std::string>{std::string{type}};
        }));
    };
    for (const char* type :
            {"run_start", "run_end", "turn_start", "turn_end", "submission", "usage_changed", "entry_appended"}) {
        counts.emplace(type, number_json(type_count(type)));
    }
    check_value(Json{std::move(counts)}, "counts", *frozen);

    const auto first_of = [&events](const std::string_view type) -> const Json* {
        for (const Json& event : events) {
            if (string_of(event, "type") == std::optional<std::string>{std::string{type}}) return &event;
        }
        return nullptr;
    };
    const Json* tool_start = first_of("tool_execution_start");
    const Json* tool_end = first_of("tool_execution_end");
    REQUIRE(tool_start != nullptr);
    REQUIRE(tool_end != nullptr);
    check_value(summarize(*tool_start), "toolStart", *frozen);
    check_value(summarize(*tool_end), "toolEnd", *frozen);

    std::vector<std::string> after_tool_end;
    bool seen_end = false;
    for (const Json& event : events) {
        const std::string type = string_of(event, "type").value_or("");
        if (seen_end && after_tool_end.size() < 2) after_tool_end.push_back(type);
        if (type == "tool_execution_end") seen_end = true;
    }
    Array after;
    for (const std::string& type : after_tool_end)
        after.emplace_back(type);
    check_value(Json{std::move(after)}, "afterToolEnd", *frozen);

    // Linkage: the placed submission is the run input, the settled submission
    // is that same record, and the tool end names the result entry its
    // following message reports.
    const Json* run_start = first_of("run_start");
    const Json* run_end = first_of("run_end");
    const Json* placed = nullptr;
    const Json* done = nullptr;
    for (const Json& event : events) {
        if (string_of(event, "type") != std::optional<std::string>{"submission"}) continue;
        const Json* record = member_of(event, "record");
        if (record == nullptr) continue;
        if (string_of(*record, "status") == std::optional<std::string>{"placed"}) placed = record;
        if (string_of(*record, "status") == std::optional<std::string>{"done"}) done = record;
    }
    REQUIRE(run_start != nullptr);
    REQUIRE(run_end != nullptr);
    REQUIRE(placed != nullptr);
    REQUIRE(done != nullptr);
    const auto input_id = [](const Json& marker) -> std::optional<double> {
        const Json* inputs = member_of(marker, "inputs");
        if (inputs == nullptr || inputs->as_array() == nullptr || inputs->as_array()->empty()) return std::nullopt;
        if (inputs->as_array()->front().as_number() == nullptr) return std::nullopt;
        return *inputs->as_array()->front().as_number();
    };
    Object linkage{{"placedSubmissionIsRunInput", Json(number_of(*placed, "id") == input_id(*run_start))},
            {"runEndInputsMatchRunStart", Json(*member_of(*run_end, "inputs") == *member_of(*run_start, "inputs"))},
            {"settledSubmissionIsRunInput", Json(number_of(*done, "id") == number_of(*placed, "id"))}};
    const Json* tool_result_end = nullptr;
    for (const Json& event : events) {
        if (string_of(event, "type") != std::optional<std::string>{"message_end"}) continue;
        const Json* entry = member_of(event, "entry");
        if (entry != nullptr && string_of(*entry, "kind") == std::optional<std::string>{"pi.tool-result"}) {
            tool_result_end = entry;
            break;
        }
    }
    REQUIRE(tool_result_end != nullptr);
    const Json* end_entry = member_of(*tool_end, "entry");
    REQUIRE(end_entry != nullptr);
    linkage.emplace(
            "toolEndEntryIsResultMessage", Json(*member_of(*end_entry, "id") == *member_of(*tool_result_end, "id")));
    check_value(Json{std::move(linkage)}, "linkage", *frozen);

    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(kinds, "entryKinds", *frozen);
    check_value(Json{Object{{"reason", Json(std::string{end == WatchEndReason::Stopped ? "stopped" : "other"})}}},
            "stop",
            *frozen);
}

TEST_CASE("a tool held by a gate keeps its slot running and a late subscriber sees committed state only",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(kEventArtifact,
            "a tool held by a gate keeps its slot running and a later subscriber sees committed state only");
    REQUIRE(frozen.has_value());
    auto gate = std::make_shared<Gate>();
    Registry registry;
    Extension extensions{.name = "event-tools"};
    extensions.tools.push_back(gated_print_tool(gate));
    REQUIRE(registry.install(std::move(extensions)).has_value());
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({
                    ScriptedReply{.message = tool_call_message(
                                          "print", support::JsonValue{support::JsonValue::object_t{{"n", 1.0}}}, "c1")},
                    ScriptedReply{.message = answer_message("done")},
            }));
    auto stream = attach_events(*opened.harness, opened.root);
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    const bool held = wait_for_event(collector, "tool_execution_update");
    const auto held_batches = collector->batches();

    auto later = attach_events(*opened.harness, opened.root);
    const Json held_snapshot = snapshot_summary(later.snapshot());
    auto later_collector = std::make_shared<EventCollector>();
    REQUIRE(later.start(collector_listener(later_collector)).has_value());
    const Json late_during_hold = batch_types(later_collector->batches());
    const std::size_t late_during_hold_count = later_collector->size();
    auto inspected = consume(opened.harness->inspect());
    const auto later_end = later.stop();
    gate->release();
    auto settled = consume(admitted.value().wait());
    const bool delivered_run_end = wait_for_event(collector, "run_end");
    const Json after_release = batch_types(collector->batches());
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(held);
    REQUIRE(inspected.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered_run_end);
    check_value(held_snapshot, "heldSnapshot", *frozen);
    check_value(task_states(inspected.value()), "heldTasks", *frozen);
    check_value(collapse_batch_types(batch_types(held_batches)), "heldBatchesCollapsed", *frozen);
    // A stream that starts after the held commits receives only later batches.
    CHECK(late_during_hold_count == 0);
    check_value(late_during_hold, "lateDuringHold", *frozen);
    check_value(collapse_batch_types(after_release), "afterReleaseCollapsed", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    CHECK(later_end == WatchEndReason::Stopped);
    check_value(Json{Object{{"reason", Json(std::string{"stopped"})}}}, "lateEnd", *frozen);
}

TEST_CASE("cancelling a run during its tool settles the input unanswered and ends the tool with a result",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(kEventArtifact,
            "cancelling a run during its tool settles the input unanswered and ends the tool with a result");
    REQUIRE(frozen.has_value());
    auto gate = std::make_shared<Gate>();
    Registry registry;
    Extension extensions{.name = "event-tools"};
    extensions.tools.push_back(gated_print_tool(gate));
    REQUIRE(registry.install(std::move(extensions)).has_value());
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({
                    ScriptedReply{.message = tool_call_message(
                                          "print", support::JsonValue{support::JsonValue::object_t{{"n", 1.0}}}, "c1")},
                    ScriptedReply{.message = answer_message("done")},
                    ScriptedReply{.message = answer_message("after abort")},
            }));
    auto stream = attach_events(*opened.harness, opened.root);
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    const bool started = collector->wait_until([](const std::vector<std::vector<Json>>& batches) {
        for (const auto& batch : batches) {
            for (const Json& event : batch) {
                if (string_of(event, "type") == std::optional<std::string>{"tool_execution_update"} &&
                        member_of(event, "details") != nullptr) {
                    return true;
                }
            }
        }
        return false;
    });
    auto aborted = consume(opened.harness->abort_conversation(opened.root));
    gate->release();
    auto settled = consume(admitted.value().wait());
    const bool delivered_run_end = wait_for_event(collector, "run_end");
    const Json cancelled_batches = collapse_batch_types(batch_types(collector->batches()));
    const auto events = collector->flattened();
    Array summary_events;
    for (const Json& event : events)
        summary_events.emplace_back(summarize(event));
    auto inspected = consume(opened.harness->inspect());
    auto second = consume(opened.harness->submit(opened.root, text_input("again")));
    std::optional<Outcome<SettledSubmission>> second_settled;
    if (second.has_value()) second_settled.emplace(consume(second.value().wait()));
    const bool delivered_second_end = second_settled.has_value() && wait_for_event_count(collector, "run_end", 2);
    const Json after_second = collapse_batch_types(batch_types(collector->batches()));
    const Json kinds = entry_kinds(storage, opened.root);
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(started);
    REQUIRE(aborted.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered_run_end);
    REQUIRE(inspected.has_value());
    REQUIRE(second.has_value());
    REQUIRE(second_settled.has_value());
    REQUIRE(second_settled->has_value());
    REQUIRE(delivered_second_end);
    check_value(cancelled_batches, "cancelledBatches", *frozen);
    (void)summary_events;
    // A details report the frozen pacing publishes only after the abort is not
    // part of the compared progress.
    Array cleaned;
    const Json collapsed = collapse_updates(events);
    if (const auto* listed = collapsed.as_array(); listed != nullptr) {
        for (const Json& event : *listed) {
            if (const auto* fields = event.as_object(); fields != nullptr && fields->contains("details")) {
                Object copy = *fields;
                copy.erase("details");
                cleaned.emplace_back(Json{std::move(copy)});
            } else {
                cleaned.emplace_back(event);
            }
        }
    }
    check_value(Json{std::move(cleaned)}, "collapsedEvents", *frozen);
    const Json* tool_end = nullptr;
    std::size_t tool_end_index = 0;
    std::size_t run_end_index = 0;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const std::string type = string_of(events[index], "type").value_or("");
        if (type == "tool_execution_end" && tool_end == nullptr) {
            tool_end = &events[index];
            tool_end_index = index;
        }
        if (type == "run_end" && run_end_index == 0) run_end_index = index;
    }
    REQUIRE(tool_end != nullptr);
    check_value(summarize(*tool_end), "toolEnd", *frozen);
    CHECK(tool_end_index < run_end_index);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(task_states(inspected.value()), "abortedTasks", *frozen);
    check_value(settled_summary(second_settled->value()), "secondSettled", *frozen);
    check_value(after_second, "afterSecond", *frozen);
    check_value(kinds, "entryKinds", *frozen);
}

TEST_CASE("a retryable provider error reports retry start and end around the recovered answer",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(
            kEventArtifact, "a retryable provider error reports retry start and end around the recovered answer");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.settings.retry.enabled = true;
    options.settings.retry.max_retries = 1;
    options.settings.retry.base_delay_ms = 1;
    options.settings.retry.max_agent_delay_ms = 50;
    auto retryable = ai::AssistantMessage{};
    retryable.stop_reason = ai::AssistantStopReason::Error;
    retryable.error_message = "rate limit exceeded";
    options.stream = scripted_stream({
            ScriptedReply{.message = std::move(retryable),
                    .failure = ai::InferenceFailure{.kind = ai::InferenceFailureKind::RateLimited}},
            ScriptedReply{.message = answer_message("recovered")},
    });
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(test_root_options()));
    REQUIRE(root.has_value());
    auto stream = attach_events(*harness, root.value());
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("go")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    const bool delivered_run_end = wait_for_event(collector, "run_end");
    const auto events = collector->flattened();
    Array summaries;
    for (const Json& event : events)
        summaries.emplace_back(summarize(event));
    const Json batches = batch_types(collector->batches());
    const auto closed = consume(harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered_run_end);
    check_value(batches, "batches", *frozen);
    check_value(Json{std::move(summaries)}, "events", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    const auto retry_start = std::ranges::find_if(events, [](const Json& event) {
        return string_of(event, "type") == std::optional<std::string>{"auto_retry_start"};
    });
    const auto retry_end = std::ranges::find_if(events,
            [](const Json& event) { return string_of(event, "type") == std::optional<std::string>{"auto_retry_end"}; });
    REQUIRE(retry_start != events.end());
    REQUIRE(retry_end != events.end());
    CHECK(retry_start < retry_end);
}

TEST_CASE("a terminal provider error settles the input unanswered and ends the turn",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(kEventArtifact,
            "a terminal provider error settles the input unanswered and ends the turn without a remaining task");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    auto failure = ai::AssistantMessage{};
    failure.stop_reason = ai::AssistantStopReason::Error;
    failure.error_message = "provider exploded";
    options.stream = scripted_stream({
            ScriptedReply{.message = std::move(failure)},
            ScriptedReply{.message = answer_message("after failure")},
    });
    auto opened = consume(Harness::open(storage, std::move(options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root(test_root_options()));
    REQUIRE(root.has_value());
    auto stream = attach_events(*harness, root.value());
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(harness->submit(root.value(), text_input("go")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    const bool delivered_run_end = wait_for_event(collector, "run_end");
    const auto events = collector->flattened();
    Array summaries;
    for (const Json& event : events)
        summaries.emplace_back(summarize(event));
    const Json batches = batch_types(collector->batches());
    auto inspected = consume(harness->inspect());
    auto second = consume(harness->submit(root.value(), text_input("again")));
    std::optional<Outcome<SettledSubmission>> second_settled;
    if (second.has_value()) second_settled.emplace(consume(second.value().wait()));
    const auto closed = consume(harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered_run_end);
    REQUIRE(inspected.has_value());
    REQUIRE(second.has_value());
    REQUIRE(second_settled.has_value());
    REQUIRE(second_settled->has_value());
    check_value(batches, "failedBatches", *frozen);
    check_value(Json{std::move(summaries)}, "events", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(task_states(inspected.value()), "afterTasks", *frozen);
    check_value(settled_summary(second_settled->value()), "secondSettled", *frozen);
}

TEST_CASE("a tool that throws settles an error result on the stream and the round continues",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(
            kEventArtifact, "a tool that throws settles an error result on the stream and the round continues");
    REQUIRE(frozen.has_value());
    Registry registry;
    Extension extensions{.name = "event-tools"};
    extensions.tools.push_back(throwing_tool());
    REQUIRE(registry.install(std::move(extensions)).has_value());
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({
                    ScriptedReply{.message = tool_call_message(
                                          "print", support::JsonValue{support::JsonValue::object_t{{"n", 1.0}}}, "c1")},
                    ScriptedReply{.message = answer_message("done")},
            }));
    auto stream = attach_events(*opened.harness, opened.root);
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    const bool delivered_run_end = wait_for_event(collector, "run_end");
    const auto events = collector->flattened();
    Array summaries;
    for (const Json& event : events)
        summaries.emplace_back(summarize(event));
    const Json batches = batch_types(collector->batches());
    const Json* tool_end_event = nullptr;
    for (const Json& event : events) {
        if (string_of(event, "type") == std::optional<std::string>{"tool_execution_end"}) tool_end_event = &event;
    }
    const Json kinds = entry_kinds(storage, opened.root);
    auto entries = storage.scan_entries(opened.root, SIZE_MAX);
    const Json observed_result = [&entries]() -> Json {
        if (!entries.has_value()) return Json{};
        std::ranges::reverse(*entries);
        const Json* result = nullptr;
        for (const EntryRecord& entry : *entries) {
            if (entry.kind == "pi.tool-result") {
                result = &entry.data;
                break;
            }
        }
        if (result == nullptr) return Json{};
        const Json* message = nullptr;
        if (const Json* model = member_of(*result, "model"); model != nullptr && model->as_array() != nullptr) {
            message = &model->as_array()->front();
        }
        if (message == nullptr) return Json{};
        Array content_types;
        std::string text;
        if (const Json* content = member_of(*message, "content");
                content != nullptr && content->as_array() != nullptr) {
            for (const Json& block : *content->as_array()) {
                content_types.emplace_back(string_of(block, "type").value_or(""));
                text += string_of(block, "text").value_or("");
            }
        }
        return Json{Object{{"toolCallId", Json(string_of(*message, "toolCallId").value_or(""))},
                {"toolName", Json(string_of(*message, "toolName").value_or(""))},
                {"isError",
                        Json(member_of(*message, "isError") != nullptr &&
                                member_of(*message, "isError")->as_boolean() != nullptr &&
                                *member_of(*message, "isError")->as_boolean())},
                {"contentTypes", Json{std::move(content_types)}},
                {"text", Json(text)}}};
    }();
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered_run_end);
    REQUIRE(tool_end_event != nullptr);
    check_value(batches, "batches", *frozen);
    check_value(Json{std::move(summaries)}, "events", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(summarize(*tool_end_event), "toolEnd", *frozen);
    check_value(observed_result, "result", *frozen);
    check_value(kinds, "entryKinds", *frozen);
}

TEST_CASE("stopping a started stream ends its deliveries and leaves persistent work running",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(
            kEventArtifact, "stopping a started stream ends its deliveries and leaves persistent work running");
    REQUIRE(frozen.has_value());
    auto gate = std::make_shared<Gate>();
    Registry registry;
    Extension extensions{.name = "event-tools"};
    extensions.tools.push_back(gated_print_tool(gate));
    REQUIRE(registry.install(std::move(extensions)).has_value());
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({
                    ScriptedReply{.message = tool_call_message(
                                          "print", support::JsonValue{support::JsonValue::object_t{{"n", 1.0}}}, "c1")},
                    ScriptedReply{.message = answer_message("done")},
            }));
    auto stream = attach_events(*opened.harness, opened.root);
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    const bool started = wait_for_event(collector, "tool_execution_update");
    const std::size_t delivered_before_stop = collector->size();
    const auto end = stream.stop();
    const auto again = stream.stop();
    gate->release();
    auto settled = consume(admitted.value().wait());
    const std::size_t delivered_after_stop = collector->size();
    const Json kinds = entry_kinds(storage, opened.root);
    const auto restarted = stream.start(collector_listener(collector));

    auto unstarted = attach_events(*opened.harness, opened.root);
    const Json buffered_message{Object{{"role", Json(std::string{"user"})},
            {"content", Json(std::string{"buffered"})},
            {"timestamp", number_json(0)}}};
    const auto committed = consume(opened.harness->session().commit(
            [conversation = opened.root, buffered_message](Transaction& tx) -> support::ExpectedVoid {
                auto entry = tx.append_entry(conversation,
                        "pi.user",
                        Json{Object{{"model", Json{Array{buffered_message}}}}},
                        std::nullopt,
                        {buffered_message});
                if (!entry) return std::unexpected(entry.error());
                return {};
            }));
    const auto unstarted_end = unstarted.stop();
    const auto unstarted_started = unstarted.start(collector_listener(collector));
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(started);
    REQUIRE(settled.has_value());
    REQUIRE(committed.has_value());
    CHECK(delivered_after_stop == delivered_before_stop);
    CHECK(delivered_before_stop > 0);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(kinds, "entryKinds", *frozen);
    check_value(Json(!restarted.has_value()), "restartRejected", *frozen);
    check_value(Json(!unstarted_started.has_value()), "unstartedStartRejected", *frozen);
    check_value(
            number_json(static_cast<std::int64_t>(delivered_after_stop - delivered_before_stop)), "stopGain", *frozen);
    check_value(Json(std::string{"stopped"}), "cancelledReason", *frozen);
    check_value(number_json(static_cast<std::int64_t>(0)), "unstartedBatches", *frozen);
    CHECK(end == WatchEndReason::Stopped);
    CHECK(again == WatchEndReason::Stopped);
    CHECK(unstarted_end == WatchEndReason::Stopped);
    check_value(Json{Object{{"reason", Json(std::string{"stopped"})}}}, "end", *frozen);
    check_value(Json{Object{{"reason", Json(std::string{"stopped"})}}}, "unstartedEnd", *frozen);
}

TEST_CASE("an unstarted stream buffers committed batches and replays them when it starts",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(
            kEventArtifact, "an unstarted stream buffers committed batches and replays them when it starts");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    auto opened = open_harness(storage, registry, scripted_stream({ScriptedReply{.message = answer_message("done")}}));
    auto stream = attach_events(*opened.harness, opened.root);
    const Json attached = snapshot_summary(stream.snapshot());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    auto collector = std::make_shared<EventCollector>();
    const auto started = stream.start(collector_listener(collector));
    const Json after_start = snapshot_summary(stream.snapshot());
    const bool delivered =
            collector->wait_until([](const std::vector<std::vector<Json>>& batches) { return !batches.empty(); });

    auto late = attach_events(*opened.harness, opened.root);
    const Json late_snapshot = snapshot_summary(late.snapshot());
    auto late_collector = std::make_shared<EventCollector>();
    const auto late_started = late.start(collector_listener(late_collector));
    const std::size_t late_count = late_collector->size();
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(started.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(delivered);
    REQUIRE(late_started.has_value());
    check_value(attached, "attached", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(batch_types(collector->batches()), "delayedBatches", *frozen);
    check_value(after_start, "afterStart", *frozen);
    check_value(late_snapshot, "lateSnapshot", *frozen);
    CHECK(late_count == 0);
    check_value(batch_types(late_collector->batches()), "lateDelivered", *frozen);
}

TEST_CASE("more than one hundred undelivered batches collapse into one snapshot replacement",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(
            kEventArtifact, "more than one hundred undelivered batches collapse into one snapshot replacement");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    auto opened = open_harness(storage, registry, scripted_stream({ScriptedReply{.message = answer_message("done")}}));
    auto append_note = [&opened](const std::string& text) {
        auto committed = consume(opened.harness->session().commit([conversation = opened.root, text](
                                                                          Transaction& tx) -> support::ExpectedVoid {
            const Json message{Object{
                    {"role", Json(std::string{"user"})}, {"content", Json(text)}, {"timestamp", number_json(0)}}};
            auto entry = tx.append_entry(
                    conversation, "pi.user", Json{Object{{"model", Json{Array{message}}}}}, std::nullopt, {message});
            if (!entry) return std::unexpected(entry.error());
            return {};
        }));
        return committed.has_value();
    };
    auto stream = attach_events(*opened.harness, opened.root);
    bool appended = true;
    for (int index = 0; index < 101; ++index)
        appended = append_note("note " + std::to_string(index)) && appended;
    auto collector = std::make_shared<EventCollector>();
    const auto started = stream.start(collector_listener(collector));
    const bool delivered =
            collector->wait_until([](const std::vector<std::vector<Json>>& batches) { return !batches.empty(); });
    const auto overflow_batches = collector->batches();

    auto short_stream = attach_events(*opened.harness, opened.root);
    bool appended_short = true;
    for (int index = 0; index < 3; ++index)
        appended_short = append_note("short " + std::to_string(index)) && appended_short;
    auto short_collector = std::make_shared<EventCollector>();
    const auto short_started = short_stream.start(collector_listener(short_collector));
    const bool short_delivered =
            short_collector->wait_until([](const std::vector<std::vector<Json>>& batches) { return !batches.empty(); });
    const auto short_batches = short_collector->batches();
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(appended);
    REQUIRE(appended_short);
    REQUIRE(started.has_value());
    REQUIRE(short_started.has_value());
    REQUIRE(delivered);
    REQUIRE(short_delivered);
    check_value(batch_types(overflow_batches), "delivered", *frozen);
    const auto& replacement_batch = overflow_batches.front();
    REQUIRE(replacement_batch.size() == 1);
    const Json& replacement = replacement_batch.front();
    const Json* entries = member_of(replacement, "entries");
    REQUIRE(entries != nullptr);
    REQUIRE(entries->as_array() != nullptr);
    std::string last_text;
    if (!entries->as_array()->empty()) {
        const Json& last = entries->as_array()->back();
        const Json* model = member_of(last, "model");
        if (model != nullptr && model->as_array() != nullptr && !model->as_array()->empty()) {
            last_text = string_of(model->as_array()->front(), "content").value_or("");
        }
    }
    const Json replacement_summary{Object{{"type", Json(string_of(replacement, "type").value_or(""))},
            {"entries", number_json(static_cast<std::int64_t>(entries->as_array()->size()))},
            {"lastText", Json(last_text)}}};
    check_value(replacement_summary, "replacement", *frozen);
    check_value(batch_types(short_batches), "shortDelivered", *frozen);
    Array short_entries;
    for (const auto& batch : short_batches) {
        for (const Json& event : batch) {
            if (string_of(event, "type") != std::optional<std::string>{"message_end"}) continue;
            const Json* entry = member_of(event, "entry");
            const Json* model = entry == nullptr ? nullptr : member_of(*entry, "model");
            if (model != nullptr && model->as_array() != nullptr && !model->as_array()->empty()) {
                short_entries.emplace_back(string_of(model->as_array()->front(), "content").value_or(""));
            }
        }
    }
    check_value(Json{std::move(short_entries)}, "shortEntries", *frozen);
}

TEST_CASE("a listener failure ends its stream and leaves the run and other subscribers running",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen = FrozenScenario::load(
            kEventArtifact, "a listener failure ends its stream and leaves the run and other subscribers running");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({ScriptedReply{.message = answer_message("done")},
                    ScriptedReply{.message = answer_message("second")}}));
    auto stream = attach_events(*opened.harness, opened.root);
    auto attempts = std::make_shared<EventCollector>();
    auto failing = [attempts](const AgentEventBatch& batch) -> support::ExpectedVoid {
        attempts->push(batch);
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "listener exploded"));
    };
    REQUIRE(stream.start(std::move(failing)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    auto settled = consume(admitted.value().wait());
    const auto end = stream.stop();
    const auto listener_error = stream.listener_error();
    const Json kinds = entry_kinds(storage, opened.root);
    const auto restarted = stream.start([](const AgentEventBatch&) -> support::ExpectedVoid { return {}; });

    auto second = attach_events(*opened.harness, opened.root);
    auto second_collector = std::make_shared<EventCollector>();
    REQUIRE(second.start(collector_listener(second_collector)).has_value());
    auto second_admitted = consume(opened.harness->submit(opened.root, text_input("again")));
    REQUIRE(second_admitted.has_value());
    auto second_settled = consume(second_admitted.value().wait());
    const bool second_delivered = wait_for_event(second_collector, "run_end");
    const Json second_batches = batch_types(second_collector->batches());
    const std::size_t second_count = second_collector->batches().size();
    const auto second_end = second.stop();
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(settled.has_value());
    REQUIRE(second_settled.has_value());
    REQUIRE(second_delivered);
    CHECK(end == WatchEndReason::ListenerError);
    check_value(Json{Object{{"reason", Json(std::string{"listener_error"})}}}, "end", *frozen);
    REQUIRE(listener_error.has_value());
    CHECK(*listener_error == "listener exploded");
    check_value(batch_types(attempts->batches()), "attempts", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(kinds, "entryKinds", *frozen);
    check_value(Json(!restarted.has_value()), "restartRejected", *frozen);
    check_value(second_batches, "secondBatches", *frozen);
    check_value(settled_summary(second_settled.value()), "secondSettled", *frozen);
    check_value(number_json(static_cast<std::int64_t>(second_count)), "secondAttempts", *frozen);
    CHECK(second_end == WatchEndReason::Stopped);
}

TEST_CASE("an aborted attachment context cancels its stream and never the run",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen =
            FrozenScenario::load(kEventArtifact, "an aborted attachment context cancels its stream and never the run");
    REQUIRE(frozen.has_value());
    Registry registry;
    MemoryStorage storage;
    auto opened = open_harness(storage, registry, scripted_stream({ScriptedReply{.message = answer_message("done")}}));
    auto stop = std::stop_source{};
    auto attached = consume(opened.harness->watch_events(opened.root, stop.get_token()));
    REQUIRE(attached.has_value());
    auto stream = std::move(attached.value());
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    const bool delivered = wait_for_event(collector, "run_end");
    stop.request_stop();
    const auto end = stream.stop();
    const std::size_t at_abort = collector->size();
    auto settled = consume(admitted.value().wait());
    const std::size_t after_run = collector->size();
    const Json kinds = entry_kinds(storage, opened.root);

    auto pre_aborted = std::stop_source{};
    pre_aborted.request_stop();
    const auto rejected = consume(opened.harness->watch_events(opened.root, pre_aborted.get_token()));
    const auto closed = consume(opened.harness->close());

    REQUIRE(closed.has_value());
    REQUIRE(delivered);
    REQUIRE(settled.has_value());
    CHECK(end == WatchEndReason::Cancelled);
    CHECK(at_abort == after_run);
    check_value(Json{Object{{"reason", Json(std::string{"cancelled"})}}}, "end", *frozen);
    check_value(number_json(static_cast<std::int64_t>(at_abort)), "atAbort", *frozen);
    check_value(number_json(static_cast<std::int64_t>(after_run)), "afterRun", *frozen);
    check_value(settled_summary(settled.value()), "settled", *frozen);
    check_value(kinds, "entryKinds", *frozen);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::Cancelled);
}

TEST_CASE("closing the Harness ends a live event stream with session_closed",
        "[durable][observation][frozen][issue1173]") {
    const auto frozen =
            FrozenScenario::load(kEventArtifact, "closing the Harness ends a live event stream with session_closed");
    REQUIRE(frozen.has_value());
    auto gate = std::make_shared<Gate>();
    Registry registry;
    Extension extensions{.name = "event-tools"};
    extensions.tools.push_back(gated_print_tool(gate));
    REQUIRE(registry.install(std::move(extensions)).has_value());
    MemoryStorage storage;
    auto opened = open_harness(storage,
            registry,
            scripted_stream({
                    ScriptedReply{.message = tool_call_message(
                                          "print", support::JsonValue{support::JsonValue::object_t{{"n", 1.0}}}, "c1")},
                    ScriptedReply{.message = answer_message("done")},
            }));
    auto stream = attach_events(*opened.harness, opened.root);
    auto collector = std::make_shared<EventCollector>();
    REQUIRE(stream.start(collector_listener(collector)).has_value());
    auto admitted = consume(opened.harness->submit(opened.root, text_input("go")));
    REQUIRE(admitted.has_value());
    const bool started = wait_for_event(collector, "tool_execution_update");
    const auto closed = consume(opened.harness->close());
    gate->release();
    const std::size_t after_close = collector->size();
    // No publication after close returns may reach the stream.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::size_t after_wait = collector->size();
    const auto end = stream.stop();

    // Release-then-assert: the Harness is closed above; the stream's terminal
    // result is read after close, which is the frozen `session_closed` path.
    REQUIRE(closed.has_value());
    REQUIRE(started);
    CHECK(after_wait == after_close);
    check_value(Json{Object{{"reason", Json(std::string{"session_closed"})}}}, "end", *frozen);
    CHECK(end == WatchEndReason::SessionClosed);
    // The frozen capture records `JSON.stringify(end)`, not the end object.
    check_value(Json(std::string{"{\"reason\":\"session_closed\"}"}), "stopAfterClose", *frozen);
    (void)admitted;
}

} // namespace cch::tests::durable
