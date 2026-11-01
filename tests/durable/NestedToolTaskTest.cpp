#include <cch/durable/Codemode.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Storage.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/ToolTask.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

constexpr auto kWait = std::chrono::seconds{10};
constexpr std::string_view kArtifact{"fixtures/pi-v1.3.0/audit-1170/codemode-nested.json"};

using Json = support::JsonValue;
using Object = Json::object_t;
using Array = Json::array_t;

const Json* field(const Json& value, std::string_view key) {
    const auto* object = value.get_if<Json::object_t>();
    if (object == nullptr) return nullptr;
    const auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}

bool nested_input(const Json& input) {
    const auto* kind = field(input, "kind");
    return kind != nullptr && kind->holds<std::string>() && kind->get_string() == "nested";
}

Json strings(const std::vector<std::string>& values) {
    Array rows;
    for (const auto& value : values)
        rows.push_back(Json{value});
    return Json{std::move(rows)};
}

/// Two independently gated nested tools: `enter()` reports the call, `release()`
/// lets one finish, and a cancelled invocation leaves through the stop token.
class Gates {
public:
    struct Waiter {
        std::mutex mutex;
        std::condition_variable_any ready;
        bool released{false};
    };

    /// Block only the first invocation of each name, so a recovered call runs to
    /// completion instead of waiting for a release that never comes.
    void block_first_only() { first_only_ = true; }

    std::shared_ptr<Waiter> enter(std::string name) {
        auto waiter = std::make_shared<Waiter>();
        {
            std::lock_guard lock(mutex_);
            entered_.push_back(name);
            waiter->released = first_only_ && ++calls_[name] > 1;
            waiters_.insert_or_assign(name, waiter);
        }
        entered_signal_.release();
        if (waiter->released) waiter->ready.notify_all();
        return waiter;
    }

    void release(std::string_view name) {
        std::shared_ptr<Waiter> waiter;
        {
            std::lock_guard lock(mutex_);
            const auto found = waiters_.find(std::string{name});
            if (found == waiters_.end()) return;
            waiter = found->second;
        }
        {
            std::lock_guard lock(waiter->mutex);
            waiter->released = true;
        }
        waiter->ready.notify_all();
    }

    void release_all() {
        std::vector<std::string> names;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [name, waiter] : waiters_)
                names.push_back(name);
        }
        for (const auto& name : names)
            release(name);
    }

    void complete(std::string name) {
        std::lock_guard lock(mutex_);
        completed_.push_back(std::move(name));
    }

    [[nodiscard]] bool wait_entered() { return entered_signal_.try_acquire_for(kWait); }

    [[nodiscard]] std::vector<std::string> entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    [[nodiscard]] std::vector<std::string> completed() const {
        std::lock_guard lock(mutex_);
        return completed_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entered_;
    std::vector<std::string> completed_;
    std::map<std::string, std::shared_ptr<Waiter>> waiters_;
    std::map<std::string, std::size_t> calls_;
    bool first_only_{false};
    std::binary_semaphore entered_signal_{0};
};

ToolRegistration gate_tool(std::shared_ptr<Gates> gates) {
    return ToolRegistration{.declaration = ai::Tool{.name = "gate", .description = "A gated nested tool"},
            .execute = [gates](support::JsonValue args,
                               std::stop_token stop) -> support::AsyncResult<std::vector<ai::Content>> {
                const auto name = args.at("name").get_string();
                auto waiter = gates->enter(name);
                {
                    std::unique_lock lock(waiter->mutex);
                    if (!waiter->ready.wait(lock, stop, [&waiter] { return waiter->released; }))
                        return support::AsyncResult<std::vector<ai::Content>>{
                                std::unexpected(support::make_error(support::ErrorCode::Cancelled, "gate aborted"))};
                }
                gates->complete(name);
                return support::AsyncResult<std::vector<ai::Content>>{support::Expected<std::vector<ai::Content>>{
                        std::vector<ai::Content>{ai::text_content(name + "-done")}}};
            },
            .replay_safe = true};
}

ai::ModelStreamFactory scripts(std::vector<std::string> sources) {
    return [sources = std::move(sources), turn = std::size_t{0}](
                   ai::Model, ai::AiContext, ai::SimpleStreamOptions) mutable {
        auto message = ai::assistant_text_message("finished");
        if (turn % 2 == 0 && turn / 2 < sources.size()) {
            message.content = {ai::ToolCallContent{.id = "c1",
                    .name = "codemode",
                    .arguments = support::JsonValue{support::JsonValue::object_t{{"code", sources[turn / 2]}}}}};
            message.stop_reason = ai::AssistantStopReason::ToolUse;
        }
        ++turn;
        return ai::ModelStream{ai::ModelStreamProducer{[message = std::move(message)](ai::AssistantEventSink sink,
                                                               ai::ModelStreamCompletion completion) mutable noexcept {
            auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
            if (!delivered)
                completion(std::unexpected(delivered.error()));
            else
                completion(std::move(message));
        }}};
    };
}

/// One open Harness over a scripted model, its root, the gated tools, and — when
/// asked — the `pi.tool` hooks the nested calls must run through.
struct Rig {
    MemoryStorage storage{};
    Registry registry{};
    std::unique_ptr<Harness> harness{};
    ConversationId root{};
    std::shared_ptr<Gates> gates = std::make_shared<Gates>();
    /// The hook log: written from nested-call task threads, read by the test.
    struct HookLog {
        std::mutex mutex;
        std::vector<std::string> entries;
        void add(std::string entry) {
            std::lock_guard lock(mutex);
            entries.push_back(std::move(entry));
        }
        [[nodiscard]] std::vector<std::string> snapshot() {
            std::lock_guard lock(mutex);
            return entries;
        }
    };
    std::shared_ptr<HookLog> hooks = std::make_shared<HookLog>();

    explicit Rig(std::string source, bool install_hooks = false) {
        Extension gated{.name = "gates"};
        gated.tools.push_back(gate_tool(gates));
        REQUIRE(registry.install(std::move(gated)).has_value());
        if (install_hooks) {
            Extension hooked{.name = "nested-hooks"};
            hooked.hooks.push_back(tool_hook("beforeTool",
                    [log = hooks](const chord::Json& input, TaskRuntime&) -> support::AsyncResult<chord::Json> {
                        const auto call = input.to_support();
                        const auto* parent = field(call, "parent");
                        if (parent == nullptr)
                            return support::AsyncResult<chord::Json>{
                                    support::Expected<chord::Json>{chord::Json{chord::Json::Object{}}}};
                        log->add(std::format(
                                "before:{}:{}", call.at("id").get_string(), field(*parent, "callId")->get_string()));
                        return support::AsyncResult<chord::Json>{
                                support::Expected<chord::Json>{chord::Json{chord::Json::Object{}}}};
                    }));
            hooked.hooks.push_back(tool_hook("afterTool",
                    [log = hooks](const chord::Json& input, TaskRuntime&) -> support::AsyncResult<chord::Json> {
                        auto value = input.to_support();
                        auto result = value.at("result");
                        const auto* call = field(value, "call");
                        if (call == nullptr || field(*call, "parent") == nullptr) {
                            // Only nested calls are rewritten; the model's own call keeps its result.
                            auto unchanged = chord::Json::from_support(result);
                            if (!unchanged)
                                return support::AsyncResult<chord::Json>{std::unexpected(unchanged.error())};
                            return support::AsyncResult<chord::Json>{
                                    support::Expected<chord::Json>{std::move(*unchanged)}};
                        }
                        Array blocks;
                        std::string text;
                        for (const auto& block : result.at("output").get_array()) {
                            Object copy = block.get_object();
                            if (block.at("type").get_string() == "text") {
                                text += block.at("text").get_string();
                                copy.insert_or_assign("text", Json{block.at("text").get_string() + "-hooked"});
                            }
                            blocks.push_back(Json{std::move(copy)});
                        }
                        log->add(std::format("after:{}:{}", field(*call, "id")->get_string(), text));
                        Object replacement = result.get_object();
                        replacement.insert_or_assign("output", Json{std::move(blocks)});
                        auto encoded = chord::Json::from_support(Json{std::move(replacement)});
                        if (!encoded) return support::AsyncResult<chord::Json>{std::unexpected(encoded.error())};
                        return support::AsyncResult<chord::Json>{support::Expected<chord::Json>{std::move(*encoded)}};
                    }));
            REQUIRE(registry.install(std::move(hooked)).has_value());
        }
        auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path());
        REQUIRE(sandbox.has_value());
        REQUIRE(registry.install(codemode_extension(std::move(*sandbox))).has_value());
        auto opened = consume(
                Harness::open(storage, HarnessOptions{.registry = &registry, .stream = scripts({std::move(source)})}));
        REQUIRE(opened.has_value());
        harness = std::move(opened.value());
        auto created = consume(harness->root());
        REQUIRE(created.has_value());
        root = created.value();
    }

    /// Admit one input; the caller keeps the handle to wait on it.
    [[nodiscard]] std::optional<Submission> submit() {
        auto input = consume(harness->submit(root, SubmissionDraft{.body = InputSubmissionDraft{.content = "run"}}));
        if (!input.has_value()) return std::nullopt;
        return std::move(input.value());
    }

    /// The last model-visible tool result; absent when the view or the entry is
    /// missing, so the caller asserts only after the Harness is closed.
    [[nodiscard]] std::optional<support::JsonValue> last_result() const {
        auto view = consume(harness->conversation_view(root));
        if (!view.has_value()) return std::nullopt;
        const auto json = view.value().to_support();
        const auto& entries = json.at("entries").get_array();
        const auto found = std::find_if(entries.rbegin(), entries.rend(), [](const auto& entry) {
            return entry.at("kind").get_string() == "pi.tool-result";
        });
        if (found == entries.rend()) return std::nullopt;
        return found->at("data").at("model").get_array().front();
    }

    [[nodiscard]] std::optional<TaskId> live_nested_parent() const {
        auto inspection = consume(harness->inspect());
        if (!inspection.has_value()) return std::nullopt;
        for (const auto& task : inspection.value().tasks) {
            const auto input = task.record.input.to_support();
            if (!nested_input(input)) continue;
            return TaskId{.value = static_cast<std::int64_t>(field(input, "parent")->get_number())};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<support::JsonValue> live_nested() const {
        auto inspection = consume(harness->inspect());
        if (!inspection.has_value()) return {};
        std::vector<support::JsonValue> rows;
        for (const auto& task : inspection.value().tasks) {
            const auto input = task.record.input.to_support();
            if (!nested_input(input)) continue;
            const auto parent = static_cast<std::int64_t>(field(input, "parent")->get_number());
            rows.push_back(Json{Object{{"key", input.at("key")},
                    {"callId", input.at("call").at("id")},
                    {"callName", input.at("call").at("name")},
                    {"parentIsCallingTask", task.record.owner.has_value() && task.record.owner->value == parent},
                    {"ownerIsParentTask", task.record.owner.has_value() && task.record.owner->value == parent},
                    {"status", task.record.state.status}}});
        }
        std::ranges::sort(rows, [](const auto& left, const auto& right) {
            return left.at("callId").get_string() < right.at("callId").get_string();
        });
        return rows;
    }

    [[nodiscard]] std::optional<TaskId> live_nested_task(std::string_view key) const {
        auto inspection = consume(harness->inspect());
        if (!inspection.has_value()) return std::nullopt;
        for (const auto& task : inspection.value().tasks) {
            const auto input = task.record.input.to_support();
            if (!nested_input(input)) continue;
            if (field(input, "key")->get_string() != key) continue;
            return task.record.id;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t live_nested_count() const {
        auto inspection = consume(harness->inspect());
        if (!inspection.has_value()) return 0;
        return static_cast<std::size_t>(std::ranges::count_if(inspection.value().tasks,
                [](const auto& task) { return nested_input(task.record.input.to_support()); }));
    }

    /// One nested-call member, normalized to the frozen projection.
    [[nodiscard]] std::optional<support::JsonValue> nested_doc(TaskId parent, std::string_view key) const {
        auto document = consume(harness->session().snapshot(
                nested_call_document(), DocumentAddress{.task_id = parent, .key = std::string{key}}));
        if (!document.has_value() || !document.value().has_value()) return std::nullopt;
        const auto value = document.value()->to_support();
        const auto& object = value.get_object();
        const auto result = object.find("result");
        if (result == object.end())
            return Json{Object{
                    {"key", std::string{key}}, {"taskIdPresent", object.contains("taskId")}, {"result", Json{}}}};
        const auto& stored = result->second;
        Array codes;
        Array messages;
        if (const auto diagnostics = stored.get_object().find("diagnostics");
                diagnostics != stored.get_object().end()) {
            for (const auto& diagnostic : diagnostics->second.get_array()) {
                if (diagnostic.at("severity").get_string() != "error") continue;
                codes.push_back(diagnostic.at("code"));
                messages.push_back(diagnostic.at("message"));
            }
        }
        return Json{Object{{"key", std::string{key}},
                {"taskIdPresent", object.contains("taskId")},
                {"result",
                        Object{{"isError", stored.at("isError")},
                                {"codes", Json{std::move(codes)}},
                                {"messages", Json{std::move(messages)}},
                                {"durationPresent", stored.get_object().contains("durationMs")}}}}};
    }

    [[nodiscard]] std::vector<support::JsonValue> nested_docs(TaskId parent) const {
        std::vector<support::JsonValue> rows;
        for (const auto key : {"c1", "c2"}) {
            if (auto row = nested_doc(parent, key); row.has_value()) rows.push_back(std::move(*row));
        }
        return rows;
    }

    /// The terminal task records of the nested calls, normalized to the frozen projection.
    /// The terminal nested-call task records; absent when the scan fails.
    [[nodiscard]] std::optional<std::vector<support::JsonValue>> nested_records() {
        auto tasks = storage.scan_tasks(1024);
        if (!tasks.has_value()) return std::nullopt;
        std::vector<support::JsonValue> rows;
        for (const auto& task : *tasks) {
            if (task.kind != "pi.tool") continue;
            const auto input = task.input.to_support();
            if (!nested_input(input)) continue;
            const auto parent = static_cast<std::int64_t>(field(input, "parent")->get_number());
            rows.push_back(Json{Object{{"kind", task.kind},
                    {"inputKind", input.at("kind")},
                    {"key", input.at("key")},
                    {"callId", input.at("call").at("id")},
                    {"callName", input.at("call").at("name")},
                    {"ownerIsParent", task.owner.has_value() && task.owner->value == parent},
                    {"outcome", task.state.outcome.has_value() ? Json{task.state.outcome->status} : Json{}},
                    {"resultKind",
                            task.state.outcome.has_value() && task.state.outcome->result.has_value()
                                    ? task.state.outcome->result->to_support().at("kind")
                                    : Json{}}}});
        }
        std::ranges::sort(rows, [](const auto& left, const auto& right) {
            return left.at("callId").get_string() < right.at("callId").get_string();
        });
        return rows;
    }
};

/// The model-visible result projection the frozen artifact records.
support::JsonValue result_view(const support::JsonValue& result) {
    // The frozen projection joins the result's text blocks with "|".
    std::string output;
    for (const auto& item : result.at("content").get_array()) {
        if (item.at("type").get_string() != "text") continue;
        if (!output.empty()) output += '|';
        output += item.at("text").get_string();
    }
    if (const auto start = output.find("Wall time "); start != std::string::npos) {
        if (const auto end = output.find('\n', start); end != std::string::npos)
            output.replace(start, end - start, "Wall time <seconds>");
    }
    Array calls;
    if (const auto details = result.get_object().find("details"); details != result.get_object().end()) {
        for (const auto& call : details->second.at("calls").get_array()) {
            calls.push_back(Json{Object{{"id", call.at("id")},
                    {"name", call.at("name")},
                    {"args", call.at("args")},
                    {"status", call.at("status")},
                    {"durationPresent", call.get_object().contains("durationMs")}}});
        }
    }
    return Json{Object{
            {"isError", result.at("isError")}, {"output", std::move(output)}, {"calls", Json{std::move(calls)}}}};
}

/// Compare one C++ projection with the frozen scenario's own `expected` object.
void compare_with_frozen(std::string_view scenario_name, const support::JsonValue& actual) {
    const auto frozen = FrozenScenario::load(kArtifact, scenario_name);
    REQUIRE(frozen.has_value());
    const auto expected = support::canonical_json(frozen->expected());
    const auto observed = support::canonical_json(actual);
    REQUIRE(expected.has_value());
    REQUIRE(observed.has_value());
    INFO("observed: " << *observed);
    CHECK(*observed == *expected);
}

/// Poll until `done()` reports the state the frozen observation recorded, with a
/// bounded backstop so a failure cannot hang a teardown join.
template <typename Done> void until(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

} // namespace

TEST_CASE(
        "Independent nested calls run concurrently and deliver in completion order", "[durable][codemode][issue1170]") {
    Rig rig{"const a = tools.gate({name:'A'}); const b = tools.gate({name:'B'}); const [ra, rb] = "
            "await Promise.all([a, b]); return ra + ':' + rb;"};
    auto input = rig.submit();
    const bool both = rig.gates->wait_entered() && rig.gates->wait_entered();
    const auto parent = rig.live_nested_parent();
    const auto live = rig.live_nested();
    const auto live_docs = parent.has_value() ? rig.nested_docs(*parent) : std::vector<support::JsonValue>{};
    if (both) rig.gates->release("B");
    std::vector<support::JsonValue> partial_docs;
    if (parent.has_value()) {
        until([&rig, parent] {
            const auto rows = rig.nested_docs(*parent);
            return std::ranges::any_of(
                    rows, [](const support::JsonValue& row) { return !row.at("result").holds<std::nullptr_t>(); });
        });
        partial_docs = rig.nested_docs(*parent);
    }
    rig.gates->release_all();
    std::optional<SettledSubmission> settled;
    if (input.has_value()) {
        auto waited = consume(input.value().wait());
        if (waited.has_value()) settled = std::move(waited.value());
    }
    const auto result = rig.last_result();
    const auto records = rig.nested_records();
    REQUIRE(consume(rig.harness->close()).has_value());
    REQUIRE(both);
    REQUIRE(settled.has_value());
    REQUIRE(result.has_value());
    REQUIRE(records.has_value());
    compare_with_frozen("independent nested calls run concurrently and deliver in completion order",
            Json{Object{{"bothLiveBeforeRelease", both},
                    {"enteredOrder", strings(rig.gates->entered())},
                    {"completionOrder", strings(rig.gates->completed())},
                    {"liveTasks", Json{live}},
                    {"docsLive", Json{live_docs}},
                    {"docsPartial", Json{partial_docs}},
                    {"nestedTaskRecords", Json{*records}},
                    {"isError", result->at("isError")},
                    {"output", result_view(*result).at("output")},
                    {"calls", result_view(*result).at("calls")}}});
}

TEST_CASE("Cancelling one nested task leaves its sibling and the script running", "[durable][codemode][issue1170]") {
    Rig rig{"const a = tools.gate({name:'A'}); const b = tools.gate({name:'B'}); const [ra, rb] = "
            "await Promise.all([a.catch((e) => 'error:' + e.message), b.catch((e) => 'error:' + e.message)]); "
            "return ra + ':' + rb;"};
    auto input = rig.submit();
    const bool both = rig.gates->wait_entered() && rig.gates->wait_entered();
    const auto parent = rig.live_nested_parent();
    const auto target = rig.live_nested_task("c1");
    std::optional<std::string> marked;
    if (target.has_value()) {
        auto abort = consume(rig.harness->abort_task(*target));
        if (abort.has_value()) marked = std::move(abort.value());
    }
    std::vector<support::JsonValue> cancelled_docs;
    if (parent.has_value()) {
        until([&rig, parent] {
            const auto rows = rig.nested_docs(*parent);
            return std::ranges::any_of(
                    rows, [](const support::JsonValue& row) { return !row.at("result").holds<std::nullptr_t>(); });
        });
        cancelled_docs = rig.nested_docs(*parent);
    }
    rig.gates->release_all();
    std::optional<SettledSubmission> settled;
    if (input.has_value()) {
        auto waited = consume(input.value().wait());
        if (waited.has_value()) settled = std::move(waited.value());
    }
    const auto result = rig.last_result();
    REQUIRE(consume(rig.harness->close()).has_value());
    REQUIRE(both);
    REQUIRE(settled.has_value());
    REQUIRE(marked.has_value());
    REQUIRE(result.has_value());
    compare_with_frozen("cancelling one nested task leaves its sibling and the script running",
            Json{Object{{"abortResult", *marked},
                    {"completionOrder", strings(rig.gates->completed())},
                    {"docsAfterCancel", Json{cancelled_docs}},
                    {"isError", result->at("isError")},
                    {"output", result_view(*result).at("output")},
                    {"calls", result_view(*result).at("calls")}}});
}

TEST_CASE("Cancelling the parent codemode call aborts its nested work", "[durable][codemode][issue1170]") {
    Rig rig{"await tools.gate({name:'A'}); store('kept', 'yes'); return 'never';"};
    auto input = rig.submit();
    const bool entered = rig.gates->wait_entered();
    const auto parent = rig.live_nested_parent();
    // This runtime executes a model-issued call inside the generation task, so the
    // generation task owns the script's nested calls and its abort cascades to
    // them (frozen pi owns them through the model call's `pi.tool` task).
    std::optional<std::string> marked;
    if (parent.has_value()) {
        auto abort = consume(rig.harness->abort_task(*parent));
        if (abort.has_value()) marked = std::move(abort.value());
    }
    rig.gates->release_all();
    std::optional<SettledSubmission> settled;
    if (input.has_value()) {
        auto waited = consume(input.value().wait());
        if (waited.has_value()) settled = std::move(waited.value());
    }
    const auto result = rig.last_result();
    const auto store = consume(
            rig.harness->session().snapshot(codemode_store_document(), DocumentAddress{.conversation_id = rig.root}));
    const auto live_nested = rig.live_nested_count();
    REQUIRE(consume(rig.harness->close()).has_value());
    REQUIRE(entered);
    REQUIRE(settled.has_value());
    REQUIRE(marked.has_value());
    REQUIRE(result.has_value());
    REQUIRE(store.has_value());
    REQUIRE(store.value().has_value());
    const auto view = result_view(*result);
    compare_with_frozen("cancelling the parent codemode call aborts its nested work",
            Json{Object{{"abortResult", *marked},
                    {"storeValues", store.value()->to_support().at("values")},
                    {"liveNestedTasks", static_cast<double>(live_nested)},
                    {"isError", result->at("isError")},
                    {"output", view.at("output")},
                    {"calls", view.at("calls")}}});
}

TEST_CASE("Cancelling an observer wait does not cancel the nested call", "[durable][codemode][issue1170]") {
    Rig rig{"const a = await tools.gate({name:'A'}); return a;"};
    auto input = rig.submit();
    const bool entered = rig.gates->wait_entered();
    const auto nested = rig.live_nested_task("c1");
    std::optional<std::string> wait_error;
    if (nested.has_value()) {
        std::stop_source cancel;
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        support::Expected<TaskRecord> outcome = std::unexpected(support::Error{});
        std::move(rig.harness->wait_for_task(*nested, cancel.get_token()))
                .start([&mutex, &ready, &done, &outcome](support::Expected<TaskRecord> result) noexcept {
                    std::lock_guard lock(mutex);
                    outcome = std::move(result);
                    done = true;
                    ready.notify_one();
                });
        cancel.request_stop();
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, kWait, [&done] { return done; })) wait_error = "the wait did not settle";
        if (wait_error == std::nullopt && !outcome.has_value()) wait_error = outcome.error().message;
    }
    rig.gates->release_all();
    std::optional<SettledSubmission> settled;
    if (input.has_value()) {
        auto waited = consume(input.value().wait());
        if (waited.has_value()) settled = std::move(waited.value());
    }
    const auto result = rig.last_result();
    const auto records = rig.nested_records();
    REQUIRE(consume(rig.harness->close()).has_value());
    REQUIRE(entered);
    REQUIRE(settled.has_value());
    REQUIRE(nested.has_value());
    REQUIRE(wait_error.has_value());
    REQUIRE(result.has_value());
    REQUIRE(records.has_value());
    REQUIRE(records->size() == 1);
    const auto view = result_view(*result);
    compare_with_frozen("cancelling an observer wait does not cancel the nested call",
            Json{Object{{"waitRejected", true},
                    {"nestedOutcome", records->front().at("outcome")},
                    {"isError", result->at("isError")},
                    {"output", view.at("output")},
                    {"calls", view.at("calls")}}});
}

TEST_CASE("Nested calls use the public tool hooks with their parent", "[durable][codemode][issue1170]") {
    Rig rig{"const a = tools.gate({name:'A'}); const b = tools.gate({name:'B'}); const [ra, rb] = "
            "await Promise.all([a, b]); return ra + ':' + rb;",
            true};
    auto input = rig.submit();
    const bool both = rig.gates->wait_entered() && rig.gates->wait_entered();
    // The frozen observation released B first: the nested calls end in that order.
    rig.gates->release("B");
    until([&rig] {
        const auto entries = rig.hooks->snapshot();
        return std::ranges::any_of(entries, [](const std::string& entry) { return entry.starts_with("after:c1/c2:"); });
    });
    rig.gates->release("A");
    std::optional<SettledSubmission> settled;
    if (input.has_value()) {
        auto waited = consume(input.value().wait());
        if (waited.has_value()) settled = std::move(waited.value());
    }
    const auto result = rig.last_result();
    const auto logged = rig.hooks->snapshot();
    REQUIRE(consume(rig.harness->close()).has_value());
    REQUIRE(both);
    REQUIRE(settled.has_value());
    REQUIRE(result.has_value());
    const auto view = result_view(*result);
    Array hook_log;
    for (const auto& entry : logged) {
        if (!entry.starts_with("before:")) continue;
        const auto separator = entry.find(':', std::string_view{"before:"}.size());
        REQUIRE(separator != std::string::npos);
        hook_log.push_back(Json{Object{{"hook", "beforeTool"},
                {"callId",
                        entry.substr(
                                std::string_view{"before:"}.size(), separator - std::string_view{"before:"}.size())},
                {"parentCallId", entry.substr(separator + 1)}}});
    }
    for (const auto& entry : logged) {
        if (!entry.starts_with("after:")) continue;
        const auto separator = entry.find(':', std::string_view{"after:"}.size());
        REQUIRE(separator != std::string::npos);
        hook_log.push_back(Json{Object{{"hook", "afterTool"},
                {"callId",
                        entry.substr(std::string_view{"after:"}.size(), separator - std::string_view{"after:"}.size())},
                {"isError", false},
                {"text", Json{Array{entry.substr(separator + 1)}}}}});
    }
    compare_with_frozen("nested calls use the public tool hooks with their parent",
            Json{Object{{"hookLog", Json{std::move(hook_log)}},
                    {"isError", result->at("isError")},
                    {"output", view.at("output")},
                    {"calls", view.at("calls")}}});
}

TEST_CASE("A script that ends without awaiting its nested call admits no call", "[durable][codemode][issue1170]") {
    Rig rig{"const pending = tools.gate({name:'A'}); return 'done';"};
    auto input = rig.submit();
    std::optional<SettledSubmission> settled;
    if (input.has_value()) {
        auto waited = consume(input.value().wait());
        if (waited.has_value()) settled = std::move(waited.value());
    }
    const auto result = rig.last_result();
    const auto records = rig.nested_records();
    REQUIRE(consume(rig.harness->close()).has_value());
    REQUIRE(settled.has_value());
    REQUIRE(result.has_value());
    REQUIRE(records.has_value());
    const auto view = result_view(*result);
    compare_with_frozen("a script that ends without awaiting its nested call admits no call",
            Json{Object{{"entered", strings(rig.gates->entered())},
                    {"nestedTasks", static_cast<double>(records->size())},
                    {"isError", result->at("isError")},
                    {"output", view.at("output")},
                    {"calls", view.at("calls")}}});
}

TEST_CASE("A recovered nested call applies its recorded replay policy", "[durable][codemode][issue1170]") {
    const bool replay_safe = GENERATE(true, false);
    CAPTURE(replay_safe);
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "nested-recovery.sqlite";
    auto storage = SqliteStorage::open(path);
    REQUIRE(storage.has_value());
    auto gates = std::make_shared<Gates>();
    gates->block_first_only();
    Registry registry;
    Extension gated{.name = "gates"};
    auto tool = gate_tool(gates);
    tool.replay_safe = replay_safe;
    gated.tools.push_back(std::move(tool));
    REQUIRE(registry.install(std::move(gated)).has_value());
    auto sandbox = codemode::CodemodeSandbox::create(codemode::default_codemode_guest_wasm_path());
    REQUIRE(sandbox.has_value());
    REQUIRE(registry.install(codemode_extension(std::move(*sandbox))).has_value());
    auto opened = consume(Harness::open(**storage,
            HarnessOptions{.registry = &registry,
                    .stream = scripts({"const a = await tools.gate({name:'A'}); store('kept', a); return a;"})}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto input =
            consume(harness->submit(root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = "run"}}));
    REQUIRE(input.has_value());
    // The interrupted invocation ends with the tool still running; the close
    // leaves the nested task owned by its parent.
    const bool entered = gates->wait_entered();
    REQUIRE(consume(harness->close()).has_value());
    harness.reset();
    REQUIRE((*storage)->close().has_value());
    storage->reset();
    storage = SqliteStorage::open(path);
    REQUIRE(storage.has_value());
    opened = consume(Harness::open(**storage,
            HarnessOptions{.registry = &registry,
                    .stream = scripts({"const a = await tools.gate({name:'A'}); store('kept', a); return a;"})}));
    REQUIRE(opened.has_value());
    harness = std::move(opened.value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(consume(harness->wait_for_idle(root.value())).has_value());
    const auto result = consume(harness->conversation_view(root.value()));
    REQUIRE(result.has_value());
    const auto json = result.value().to_support();
    const auto& entries = json.at("entries").get_array();
    const auto found = std::find_if(entries.rbegin(), entries.rend(), [](const auto& entry) {
        return entry.at("kind").get_string() == "pi.tool-result";
    });
    REQUIRE(found != entries.rend());
    const auto tool_result = found->at("data").at("model").get_array().front();
    const auto text = result_view(tool_result).at("output").get_string();
    const auto completed = gates->completed().size();
    REQUIRE(consume(harness->close()).has_value());
    REQUIRE(entered);
    if (replay_safe) {
        CHECK(completed == 1);
        CHECK_FALSE(tool_result.at("isError").get_boolean());
        CHECK(text.ends_with("A-done"));
    } else {
        CHECK(completed == 0);
        CHECK(tool_result.at("isError").get_boolean());
        CHECK(text.find("Tool gate was interrupted and may have partially run") != std::string::npos);
    }
}

} // namespace cch::tests::durable
