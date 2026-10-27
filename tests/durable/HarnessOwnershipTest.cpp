// Ownership, joins, abort cascades and wait boundaries (ticket #1078).
//
// Every expectation below is read from the frozen pi v1.3.0 artifacts
// (`fixtures/pi-v1.3.0/audit-1055/ownership-abort.json`,
// `fixtures/pi-v1.3.0/audit-1055/task-lifecycle.json` and
// `fixtures/pi-v1.3.0/audit-1078/ownership-waits.json`) so the C++ Durable
// Harness/Session is judged against the frozen Runtime's own observations,
// never against values invented on the C++ side. Identifiers that are
// environment-specific in frozen messages (task and conversation ids) are
// interpolated from the C++ run, exactly like the frozen-template discipline
// in SessionTransactionTest.

#include <cch/chord/Json.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch::durable;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace cch::tests::durable {
namespace {

const std::string kOwnershipAbortArtifact = "fixtures/pi-v1.3.0/audit-1055/ownership-abort.json";
const std::string kTaskLifecycleArtifact = "fixtures/pi-v1.3.0/audit-1055/task-lifecycle.json";
const std::string kOwnershipWaitsArtifact = "fixtures/pi-v1.3.0/audit-1078/ownership-waits.json";

/// A thread-safe append log mirroring the frozen `log` arrays.
struct SyncLog {
    mutable std::mutex mutex;
    std::vector<std::string> entries;

    void push(std::string entry) {
        std::lock_guard lock(mutex);
        entries.push_back(std::move(entry));
    }

    [[nodiscard]] bool includes(const std::string& entry) const {
        std::lock_guard lock(mutex);
        for (const std::string& existing : entries) {
            if (existing == entry) return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t first_index_of(const std::string& entry) const {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i] == entry) return i;
        }
        return entries.size();
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        std::lock_guard lock(mutex);
        return entries;
    }
};

/// One gate the test thread releases; a phase races it against the invocation
/// stop token so an abort mark ends the run phase (frozen pi
/// `untilAborted(runtime.signal)` raced with a release).
struct Gate {
    // `condition_variable_any` carries the stop-token wait overloads.
    std::mutex mutex;
    std::condition_variable_any cv;
    bool open{false};

    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }

    /// Returns true when released, false when the invocation was stopped.
    [[nodiscard]] bool wait(const std::stop_token& token) {
        std::unique_lock lock(mutex);
        return cv.wait(lock, token, [this] { return open; });
    }
};

/// Park the current phase until the invocation is stopped. Mirrors the frozen
/// `await untilAborted(runtime.signal)` phases that never complete normally.
void park_until_stopped(const std::stop_token& token) {
    Gate never_released;
    static_cast<void>(never_released.wait(token));
}

/// Poll until `predicate` holds, or fail the test after a generous deadline.
/// Runs on the test thread only.
template <typename Predicate> void wait_until(const Predicate& predicate, const std::string& what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("timed out waiting for " << what);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

[[nodiscard]] chord::Json checkpoint_phase(std::string phase) {
    chord::Json::Object checkpoint;
    checkpoint.emplace("phase", std::move(phase));
    return chord::Json(std::move(checkpoint));
}

[[nodiscard]] chord::Json::Object outcome_object(const TaskOutcome& outcome) {
    chord::Json::Object object;
    object.emplace("status", outcome.status);
    if (outcome.reason.has_value()) {
        object.emplace("reason", *outcome.reason);
    }
    if (outcome.result.has_value()) {
        object.emplace("result", *outcome.result);
    }
    return object;
}

/// Read one task record through a Transaction (frozen pi `Tx.task`), for the
/// same-commit ordered-outcome reads a join's decide phase performs.
[[nodiscard]] support::Expected<std::optional<TaskRecord>> tx_task(Transaction& tx, TaskId id) { return tx.task(id); }

/// Build the gated task used by the audit-1078 background and idle scenarios:
/// the run phase races `gate` against the invocation stop token, logging
/// `{kind}:signalled` when the mark wins and `{kind}:released` when the gate
/// does; the abort handler commits `aborted/cascade`.
void add_gated_task(std::vector<TaskDefinition>& tasks,
        const std::string& kind,
        SyncLog& log,
        const std::shared_ptr<Gate>& gate,
        const std::string& result) {
    auto builder = TaskDefinitionBuilder(kind, 1);
    builder.initial([](const chord::Json&) { return checkpoint_phase("run"); });
    builder.phase(
            "run", [kind, &log, gate, result](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                log.push(kind + ":run");
                if (!gate->wait(runtime.stop_token())) {
                    log.push(kind + ":signalled");
                    return support::AsyncResult<void>{support::ExpectedVoid{}};
                }
                log.push(kind + ":released");
                return runtime.commit([result](Transaction&,
                                              const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json(result)});
                });
            });
    builder.abort([kind, &log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        log.push(kind + ":abort");
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "cascade"});
        });
    });
    auto def = builder.build();
    REQUIRE(def.has_value());
    tasks.push_back(std::move(*def));
}

/// Build the frozen `audit.join-child`: run, optional 40 ms hold, then fail or
/// complete; the abort handler settles `aborted/cascade`.
void add_join_child(std::vector<TaskDefinition>& tasks, SyncLog& log) {
    auto builder = TaskDefinitionBuilder("audit.join-child", 1);
    builder.initial([](const chord::Json&) { return checkpoint_phase("run"); });
    builder.phase("run", [&log](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto* input = task.input.as_object();
        const std::string name = *input->at("name").as_string();
        const bool fail = input->contains("fail");
        const bool wait = input->contains("wait");
        log.push("child-run-" + name);
        if (wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        if (fail) {
            return runtime.commit(
                    [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        return NextTaskState::terminal(TaskOutcome{
                                .status = "failed",
                                .error = TaskOutcomeError{.message = "boom"},
                        });
                    });
        }
        return runtime.commit(
                [name](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json(name)});
                });
    });
    builder.abort([&log](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const std::string name = *task.input.as_object()->at("name").as_string();
        log.push("child-abort-" + name);
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "cascade"});
        });
    });
    auto def = builder.build();
    REQUIRE(def.has_value());
    tasks.push_back(std::move(*def));
}

/// Build the frozen `audit.join-parent`: spawn owned children one commit each,
/// park in `waiting` with the join policy, then read the members' ordered
/// outcomes in the decide phase. `child_def` is resolved after
/// `Harness::open`, once the registry owns the definition.
void add_join_parent(std::vector<TaskDefinition>& tasks,
        SyncLog& log,
        JoinPolicy policy,
        const std::vector<std::tuple<std::string, bool, bool>>& children,
        bool with_abort_handler,
        const TaskDefinition** child_def) {
    auto builder = TaskDefinitionBuilder("audit.join-parent", 1);
    builder.initial([](const chord::Json&) { return checkpoint_phase("spawn"); });
    builder.phase("spawn",
            [&log, child_def, children, policy](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                log.push("parent-spawn");
                std::vector<TaskId> ids;
                for (const auto& [name, fail, wait] : children) {
                    chord::Json::Object input;
                    input.emplace("name", name);
                    if (fail) input.emplace("fail", true);
                    if (wait) input.emplace("wait", true);
                    auto spawned = consume(runtime.commit(
                            [child_def, input = chord::Json(std::move(input)), &ids, owner = runtime.task_id()](
                                    Transaction& tx,
                                    const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                                auto created = tx.create_task(**child_def, input, TaskOptions{.owner = owner});
                                if (!created) return std::unexpected(created.error());
                                ids.push_back(*created);
                                return std::optional<NextTaskState>{};
                            }));
                    if (!spawned.has_value()) return support::AsyncResult<void>{std::unexpected(spawned.error())};
                }
                auto parked = consume(
                        runtime.commit([&ids, policy](Transaction&,
                                               const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            chord::Json::Object checkpoint;
                            checkpoint.emplace("phase", "decide");
                            chord::Json::Array id_jsons;
                            for (const TaskId id : ids) {
                                id_jsons.push_back(chord::Json(static_cast<double>(id.value)));
                            }
                            checkpoint.emplace("ids", chord::Json(std::move(id_jsons)));
                            return NextTaskState::waiting(chord::Json(std::move(checkpoint)), ids, policy);
                        }));
                if (!parked.has_value()) return support::AsyncResult<void>{std::unexpected(parked.error())};
                log.push("parent-waiting");
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            });
    builder.phase("decide", [&log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        log.push("parent-decide");
        return runtime.commit(
                [](Transaction& tx, const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
                    const auto* checkpoint = current.state.checkpoint->as_object();
                    const auto* ids = checkpoint->at("ids").as_array();
                    chord::Json::Array outcomes;
                    for (const chord::Json& id_json : *ids) {
                        const auto member = tx_task(tx, TaskId{static_cast<std::int64_t>(*id_json.as_number())});
                        if (!member || !member->has_value() || !(*member)->state.outcome.has_value()) {
                            return std::unexpected(
                                    support::make_error(support::ErrorCode::Validation, "member outcome missing"));
                        }
                        outcomes.push_back(chord::Json((*member)->state.outcome->status));
                    }
                    return NextTaskState::terminal(
                            TaskOutcome{.status = "completed", .result = chord::Json(std::move(outcomes))});
                });
    });
    if (with_abort_handler) {
        builder.abort([&log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            log.push("parent-abort");
            return runtime.commit(
                    [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "parent"});
                    });
        });
    }
    auto def = builder.build();
    REQUIRE(def.has_value());
    tasks.push_back(std::move(*def));
}

TEST_CASE("a failFast join aborts live siblings and the parent reads ordered terminal outcomes",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipAbortArtifact,
            "a failFast join aborts live siblings and the parent reads ordered terminal outcomes");
    REQUIRE(scenario.has_value());

    SyncLog log;
    std::vector<TaskDefinition> tasks;
    add_join_child(tasks, log);
    const TaskDefinition* child_def = nullptr;
    add_join_parent(tasks,
            log,
            JoinPolicy::FailFast,
            {{"a", true, false}, {"b", false, true}, {"c", false, true}},
            true,
            &child_def);

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    child_def = harness->task_definition("audit.join-child");
    REQUIRE(child_def != nullptr);
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId parent_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.join-parent"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        parent_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    auto receipt_res = consume(harness->wait_for_task(parent_id));
    REQUIRE(receipt_res.has_value());
    const TaskRecord& receipt = receipt_res.value();

    // The parent completed with the members' ordered outcome statuses.
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == "completed");
    REQUIRE(receipt.state.outcome->result.has_value());
    const auto* expected_parent = scenario->object_member("parent");
    REQUIRE(expected_parent != nullptr);
    CHECK(json_equals(*receipt.state.outcome->result, expected_parent->at("result")));

    // Every child settled exactly as frozen: `a` failed without an abort mark;
    // the failFast-marked siblings aborted with their marks recorded.
    auto scanned_children = storage.scan_tasks(16);
    REQUIRE(scanned_children.has_value());
    std::vector<chord::Json::Object> child_jsons;
    for (const TaskRecord& record : *scanned_children) {
        if (record.kind != "audit.join-child") continue;
        chord::Json::Object child;
        child.emplace("input", *record.input.as_object()->at("name").as_string());
        child.emplace("outcome", record.state.outcome->status);
        child.emplace("abortRequested", record.abort_requested);
        child_jsons.push_back(std::move(child));
    }
    const auto* expected_children = scenario->array_member("children");
    REQUIRE(expected_children != nullptr);
    REQUIRE(child_jsons.size() == expected_children->size());
    for (std::size_t i = 0; i < child_jsons.size(); ++i) {
        CHECK(json_equals(chord::Json(std::move(child_jsons[i])), expected_children->at(i)));
    }

    // Causal log order: the parking marker and every child run follow the
    // spawn; each aborted child's abort handler ran before the parent decided.
    REQUIRE(log.includes("parent-spawn"));
    REQUIRE(log.includes("parent-waiting"));
    CHECK(log.first_index_of("parent-spawn") < log.first_index_of("parent-waiting"));
    CHECK(log.first_index_of("parent-waiting") < log.first_index_of("parent-decide"));
    CHECK(log.first_index_of("child-run-b") < log.first_index_of("child-abort-b"));
    CHECK(log.first_index_of("child-run-c") < log.first_index_of("child-abort-c"));
    CHECK(log.first_index_of("child-abort-b") < log.first_index_of("parent-decide"));
    CHECK(log.first_index_of("child-abort-c") < log.first_index_of("parent-decide"));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("an allSettled join lets surviving siblings finish and still reads ordered outcomes",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipAbortArtifact,
            "an allSettled join lets surviving siblings finish and still reads ordered outcomes");
    REQUIRE(scenario.has_value());

    SyncLog log;
    std::vector<TaskDefinition> tasks;
    add_join_child(tasks, log);
    const TaskDefinition* child_def = nullptr;
    add_join_parent(tasks, log, JoinPolicy::AllSettled, {{"b", true, false}, {"c", false, true}}, false, &child_def);

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    child_def = harness->task_definition("audit.join-child");
    REQUIRE(child_def != nullptr);
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId parent_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.join-parent"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        parent_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    auto receipt_res = consume(harness->wait_for_task(parent_id));
    REQUIRE(receipt_res.has_value());
    const TaskRecord& receipt = receipt_res.value();

    // allSettled never marks the survivor: the parent reads [failed, completed].
    REQUIRE(receipt.state.outcome.has_value());
    const auto* expected_parent = scenario->object_member("parent");
    REQUIRE(expected_parent != nullptr);
    CHECK(json_equals(*receipt.state.outcome->result, expected_parent->at("result")));

    auto scanned_children = storage.scan_tasks(16);
    REQUIRE(scanned_children.has_value());
    std::vector<chord::Json::Object> child_jsons;
    for (const TaskRecord& record : *scanned_children) {
        if (record.kind != "audit.join-child") continue;
        chord::Json::Object child;
        child.emplace("input", *record.input.as_object()->at("name").as_string());
        child.emplace("outcome", record.state.outcome->status);
        child.emplace("abortRequested", record.abort_requested);
        child_jsons.push_back(std::move(child));
    }
    const auto* expected_children = scenario->array_member("children");
    REQUIRE(expected_children != nullptr);
    REQUIRE(child_jsons.size() == expected_children->size());
    for (std::size_t i = 0; i < child_jsons.size(); ++i) {
        CHECK(json_equals(chord::Json(std::move(child_jsons[i])), expected_children->at(i)));
    }

    // No sibling was aborted: the survivor completed without an abort mark.
    CHECK_FALSE(log.includes("child-abort-b"));
    CHECK_FALSE(log.includes("child-abort-c"));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("aborting a parent runs descendants' abort handlers before the parent and preserves order",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipAbortArtifact,
            "aborting a parent runs descendants' abort handlers before the parent and preserves order");
    REQUIRE(scenario.has_value());

    SyncLog log;
    std::vector<TaskDefinition> tasks;
    {
        // Frozen `audit.cascade-child`: parks until the abort mark; the abort
        // handler settles `aborted/cascade`.
        auto builder = TaskDefinitionBuilder("audit.cascade-child", 1);
        builder.initial([](const chord::Json&) { return checkpoint_phase("run"); });
        builder.phase("run", [&log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            log.push("child-run");
            park_until_stopped(runtime.stop_token());
            return support::AsyncResult<void>{support::ExpectedVoid{}};
        });
        builder.abort([&log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            log.push("child-abort");
            return runtime.commit(
                    [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "cascade"});
                    });
        });
        auto def = builder.build();
        REQUIRE(def.has_value());
        tasks.push_back(std::move(*def));
    }

    // The parent's abort handler reads the child's settled outcome before
    // committing its own terminal record (frozen pi `runtime.outcomes`).
    const TaskDefinition* child_def = nullptr;
    TaskId parent_id;
    {
        auto builder = TaskDefinitionBuilder("audit.cascade-parent", 1);
        builder.initial([](const chord::Json&) { return checkpoint_phase("run"); });
        builder.phase("run", [&log, &child_def](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            log.push("parent-run");
            auto committed = consume(
                    runtime.commit([child_def, owner = runtime.task_id()](Transaction& tx,
                                           const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        auto created = tx.create_task(*child_def, chord::Json{}, TaskOptions{.owner = owner});
                        if (!created) return std::unexpected(created.error());
                        chord::Json::Object checkpoint;
                        checkpoint.emplace("phase", "run");
                        checkpoint.emplace("child", static_cast<double>(created->value));
                        return NextTaskState::running(chord::Json(std::move(checkpoint)));
                    }));
            if (!committed.has_value()) return support::AsyncResult<void>{std::unexpected(committed.error())};
            park_until_stopped(runtime.stop_token());
            return support::AsyncResult<void>{support::ExpectedVoid{}};
        });
        builder.abort([&log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            return runtime.commit([&log](Transaction& tx, const TaskRecord& current)
                                          -> support::Expected<std::optional<NextTaskState>> {
                const auto* checkpoint = current.state.checkpoint->as_object();
                const auto child = tx_task(tx, TaskId{static_cast<std::int64_t>(*checkpoint->at("child").as_number())});
                if (!child || !child->has_value() || !(*child)->state.outcome.has_value()) {
                    return std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "child outcome missing"));
                }
                log.push("parent-abort:" + (*child)->state.outcome->status);
                return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "parent"});
            });
        });
        auto def = builder.build();
        REQUIRE(def.has_value());
        tasks.push_back(std::move(*def));
    }

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    child_def = harness->task_definition("audit.cascade-child");
    REQUIRE(child_def != nullptr);
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.cascade-parent"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        parent_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    wait_until([&] { return log.includes("child-run"); }, "child running");

    auto marked_res = consume(harness->abort_task(parent_id));
    REQUIRE(marked_res.has_value());
    const auto expected_marked = scenario->string_member("marked");
    REQUIRE(expected_marked.has_value());
    CHECK(marked_res.value() == *expected_marked);

    auto receipt_res = consume(harness->wait_for_task(parent_id));
    REQUIRE(receipt_res.has_value());

    // The single-child ordering is deterministic: the child's abort handler
    // ran before the parent's, and the parent read the child's aborted outcome.
    const auto* expected_log = scenario->array_member("log");
    REQUIRE(expected_log != nullptr);
    const std::vector<std::string> entries = log.snapshot();
    REQUIRE(entries.size() == expected_log->size());
    for (std::size_t i = 0; i < expected_log->size(); ++i) {
        CHECK(entries[i] == expected_log->at(i).get_string());
    }

    REQUIRE(receipt_res.value().state.outcome.has_value());
    const auto* expected_parent = scenario->object_member("parent");
    REQUIRE(expected_parent != nullptr);
    CHECK(json_equals(chord::Json(outcome_object(*receipt_res.value().state.outcome)), *expected_parent));

    const auto* checkpoint = receipt_res.value().state.checkpoint->as_object();
    auto child_res =
            consume(harness->get_task(TaskId{static_cast<std::int64_t>(*checkpoint->at("child").as_number())}));
    REQUIRE(child_res.has_value());
    REQUIRE(child_res.value().has_value());
    CHECK(child_res.value()->abort_requested);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("cancelling a wait cancels only the waiter; the persistent task keeps running and completes",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipAbortArtifact,
            "cancelling a wait cancels only the waiter; the persistent task keeps running and completes");
    REQUIRE(scenario.has_value());

    SyncLog log;
    auto gate = std::make_shared<Gate>();
    std::vector<TaskDefinition> tasks;
    {
        // Frozen `audit.slow`: one "run" log entry, park on the external gate,
        // then complete with "done".
        auto builder = TaskDefinitionBuilder("audit.slow", 1);
        builder.initial([](const chord::Json&) { return checkpoint_phase("run"); });
        builder.phase("run", [&log, gate](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            log.push("run");
            static_cast<void>(gate->wait(runtime.stop_token()));
            return runtime.commit([](Transaction&,
                                          const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("done")});
            });
        });
        builder.abort([&log](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            log.push("abort");
            return runtime.commit(
                    [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "requested"});
                    });
        });
        auto def = builder.build();
        REQUIRE(def.has_value());
        tasks.push_back(std::move(*def));
    }

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId task_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit.slow"), chord::Json{}, TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    wait_until([&] { return log.includes("run"); }, "slow task running");

    // Cancel only the caller's wait: the wait rejects with the frozen
    // "stop waiting" cancellation while the task is untouched.
    std::stop_source cancel;
    Outcome<TaskRecord> wait_outcome;
    std::atomic_bool wait_done{false};
    std::thread waiter([&] {
        wait_outcome = consume(harness->wait_for_task(task_id, cancel.get_token()));
        wait_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancel.request_stop();
    wait_until([&] { return wait_done.load(); }, "the cancelled wait rejecting");
    waiter.join();

    REQUIRE_FALSE(wait_outcome.has_value());
    CHECK(wait_outcome.error().code == support::ErrorCode::Cancelled);
    const auto* expected_wait_error = scenario->object_member("waitError");
    REQUIRE(expected_wait_error != nullptr);
    CHECK(wait_outcome.error().message == expected_wait_error->at("message").get_string());

    // The persistent task is still running and carries no abort mark.
    auto after_cancel = consume(harness->get_task(task_id));
    REQUIRE(after_cancel.has_value());
    REQUIRE(after_cancel.value().has_value());
    const auto* expected_after = scenario->object_member("afterCancel");
    REQUIRE(expected_after != nullptr);
    CHECK(after_cancel.value()->state.status == expected_after->at("status").get_string());
    CHECK(after_cancel.value()->abort_requested == expected_after->at("abortRequested").get_boolean());

    // The task then completes on its own, with no abort handler ever running.
    gate->release();
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    REQUIRE(receipt_res.value().state.outcome.has_value());
    const auto* expected_outcome = scenario->object_member("outcome");
    REQUIRE(expected_outcome != nullptr);
    CHECK(receipt_res.value().state.outcome->status == expected_outcome->at("status").get_string());
    CHECK(json_equals(*receipt_res.value().state.outcome->result, expected_outcome->at("result")));

    const auto* expected_log = scenario->array_member("log");
    REQUIRE(expected_log != nullptr);
    const std::vector<std::string> entries = log.snapshot();
    REQUIRE(entries.size() == expected_log->size());
    for (std::size_t i = 0; i < expected_log->size(); ++i) {
        CHECK(entries[i] == expected_log->at(i).get_string());
    }
    CHECK_FALSE(log.includes("abort"));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a conversation abort keeps live background work while a direct abort reaches it",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(
            kOwnershipWaitsArtifact, "a conversation abort keeps live background work while a direct abort reaches it");
    REQUIRE(scenario.has_value());

    SyncLog log;
    auto foreground_gate = std::make_shared<Gate>();
    auto background_gate = std::make_shared<Gate>();
    std::vector<TaskDefinition> tasks;
    add_gated_task(tasks, "audit.bg-foreground", log, foreground_gate, "foreground");
    add_gated_task(tasks, "audit.bg-background", log, background_gate, "background");

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId foreground_id;
    TaskId background_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto foreground = tx.create_task(*harness->task_definition("audit.bg-foreground"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!foreground) return std::unexpected(foreground.error());
        foreground_id = *foreground;
        auto background = tx.create_task(*harness->task_definition("audit.bg-background"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value(), .background = true});
        if (!background) return std::unexpected(background.error());
        background_id = *background;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    wait_until([&] { return log.includes("audit.bg-foreground:run") && log.includes("audit.bg-background:run"); },
            "both tasks running");

    // The ordinary conversation abort stops at the background boundary.
    REQUIRE(consume(harness->abort_conversation(root.value())).has_value());

    auto foreground = consume(harness->get_task(foreground_id));
    REQUIRE(foreground.has_value());
    REQUIRE(foreground.value().has_value());
    auto background_after_abort = consume(harness->get_task(background_id));
    REQUIRE(background_after_abort.has_value());
    REQUIRE(background_after_abort.value().has_value());

    const auto* expected_after_abort = scenario->object_member("afterAbort");
    REQUIRE(expected_after_abort != nullptr);
    CHECK(foreground.value()->state.status == expected_after_abort->at("foregroundStatus").get_string());
    CHECK(json_equals(chord::Json(outcome_object(*foreground.value()->state.outcome)),
            expected_after_abort->at("foregroundOutcome")));
    CHECK(background_after_abort.value()->state.status == expected_after_abort->at("backgroundStatus").get_string());
    CHECK(background_after_abort.value()->abort_requested ==
            expected_after_abort->at("backgroundAbortRequested").get_boolean());

    // A direct abort of the background task reaches it, ordinary or not.
    auto marked_res = consume(harness->abort_task(background_id));
    REQUIRE(marked_res.has_value());
    auto receipt_res = consume(harness->wait_for_task(background_id));
    REQUIRE(receipt_res.has_value());
    REQUIRE(receipt_res.value().state.outcome.has_value());
    const auto* expected_after_direct = scenario->object_member("afterDirect");
    REQUIRE(expected_after_direct != nullptr);
    CHECK(marked_res.value() == expected_after_direct->at("marked").get_string());
    CHECK(json_equals(
            chord::Json(outcome_object(*receipt_res.value().state.outcome)), expected_after_direct->at("outcome")));

    const std::vector<std::string> entries = log.snapshot();
    const auto& expected_log = expected_after_direct->at("log").get_array();
    REQUIRE(entries.size() == expected_log.size());
    for (std::size_t i = 0; i < expected_log.size(); ++i) {
        CHECK(entries[i] == expected_log.at(i).get_string());
    }

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a background-crossing conversation abort reaches and waits for a live background task",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipWaitsArtifact,
            "a background-crossing conversation abort reaches and waits for a live background task");
    REQUIRE(scenario.has_value());

    SyncLog log;
    auto gate = std::make_shared<Gate>();
    std::vector<TaskDefinition> tasks;
    add_gated_task(tasks, "audit.bg-cross", log, gate, "background");

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId background_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.bg-cross"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value(), .background = true});
        if (!created) return std::unexpected(created.error());
        background_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    wait_until([&] { return log.includes("audit.bg-cross:run"); }, "background task running");

    // Crossing the boundary marks the background task and waits for it.
    REQUIRE(consume(harness->abort_conversation(root.value(), true)).has_value());

    auto record = consume(harness->get_task(background_id));
    REQUIRE(record.has_value());
    REQUIRE(record.value().has_value());
    const auto expected_status = scenario->string_member("status");
    REQUIRE(expected_status.has_value());
    CHECK(record.value()->state.status == *expected_status);
    REQUIRE(record.value()->state.outcome.has_value());
    CHECK(json_equals(
            chord::Json(outcome_object(*record.value()->state.outcome)), *scenario->object_member("outcome")));
    CHECK(record.value()->abort_requested == scenario->expected().at("abortRequested").get_boolean());

    const auto* expected_log = scenario->array_member("log");
    REQUIRE(expected_log != nullptr);
    const std::vector<std::string> entries = log.snapshot();
    REQUIRE(entries.size() == expected_log->size());
    for (std::size_t i = 0; i < expected_log->size(); ++i) {
        CHECK(entries[i] == expected_log->at(i).get_string());
    }

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("an idle wait resolves with live background work and holds on a live foreground task",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipWaitsArtifact,
            "an idle wait resolves with live background work and holds on a live foreground task");
    REQUIRE(scenario.has_value());

    SyncLog log;
    auto foreground_gate = std::make_shared<Gate>();
    auto background_gate = std::make_shared<Gate>();
    std::vector<TaskDefinition> tasks;
    add_gated_task(tasks, "audit.idle-foreground", log, foreground_gate, "foreground");
    add_gated_task(tasks, "audit.idle-background", log, background_gate, "background");
    add_gated_task(tasks, "audit.idle-other", log, background_gate, "other");

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId background_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto foreground = tx.create_task(*harness->task_definition("audit.idle-foreground"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!foreground) return std::unexpected(foreground.error());
        auto background = tx.create_task(*harness->task_definition("audit.idle-background"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value(), .background = true});
        if (!background) return std::unexpected(background.error());
        background_id = *background;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    wait_until([&] { return log.includes("audit.idle-foreground:run") && log.includes("audit.idle-background:run"); },
            "both tasks running");

    // A background-only ownerless scope is already idle while it runs.
    ConversationId other_id;
    auto other_commit = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_conversation();
        if (!created) return std::unexpected(created.error());
        other_id = created->id;
        auto task = tx.create_task(*harness->task_definition("audit.idle-other"),
                chord::Json{},
                TaskOptions{.conversation_id = other_id, .background = true});
        if (!task) return std::unexpected(task.error());
        return {};
    }));
    REQUIRE(other_commit.has_value());
    REQUIRE(other_commit.has_value());
    wait_until(
            [&] { return log.includes("audit.idle-other:run"); }, "the other conversation's background task running");

    std::atomic_bool other_idle{false};
    std::thread other_waiter([&] {
        const Outcome<void> outcome = consume(harness->wait_for_idle(other_id));
        other_idle.store(outcome.has_value());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const bool idle_observed = other_idle.load();
    other_waiter.join();
    const auto expected_background_only = scenario->string_member("backgroundOnlyIdle");
    REQUIRE(expected_background_only.has_value());
    CHECK(idle_observed == (*expected_background_only == "idle"));

    // The root scope holds on its live foreground task, then releases while
    // the background task is still live.
    std::atomic_bool root_idle{false};
    std::thread root_waiter([&] {
        const Outcome<void> outcome = consume(harness->wait_for_idle(root.value()));
        root_idle.store(outcome.has_value());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool held_before_release = !root_idle.load();
    CHECK(held_before_release == scenario->expected().at("heldBeforeRelease").get_boolean());
    foreground_gate->release();
    wait_until([&] { return root_idle.load(); }, "the root idle wait resolving");
    root_waiter.join();

    auto background_still = consume(harness->get_task(background_id));
    REQUIRE(background_still.has_value());
    REQUIRE(background_still.value().has_value());
    const auto* expected_idle_after = scenario->object_member("idleAfterForeground");
    REQUIRE(expected_idle_after != nullptr);
    CHECK(background_still.value()->state.status == expected_idle_after->at("backgroundStillLive").get_string());
    const std::vector<std::string> idle_entries = log.snapshot();
    const auto& expected_idle_log = expected_idle_after->at("log").get_array();
    REQUIRE(idle_entries.size() == expected_idle_log.size());
    for (std::size_t i = 0; i < expected_idle_log.size(); ++i) {
        CHECK(idle_entries[i] == expected_idle_log.at(i).get_string());
    }

    // The background task finishing changes nothing about the ordinary scope.
    background_gate->release();
    auto receipt_res = consume(harness->wait_for_task(background_id));
    REQUIRE(receipt_res.has_value());
    REQUIRE(receipt_res.value().state.outcome.has_value());
    const auto* expected_after_background = scenario->object_member("afterBackground");
    REQUIRE(expected_after_background != nullptr);
    CHECK(receipt_res.value().state.outcome->status ==
            expected_after_background->at("outcome").at("status").get_string());
    CHECK(json_equals(
            *receipt_res.value().state.outcome->result, expected_after_background->at("outcome").at("result")));
    auto background_after = consume(harness->get_task(background_id));
    REQUIRE(background_after.has_value());
    REQUIRE(background_after.value().has_value());
    CHECK(background_after.value()->state.status == expected_after_background->at("backgroundStillLive").get_string());

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a child task and a task-owned conversation are rejected for a terminal owner and accepted for a live one",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(kOwnershipWaitsArtifact,
            "a child task and a task-owned conversation are rejected for a terminal owner and accepted for a live one");
    REQUIRE(scenario.has_value());

    SyncLog log;
    auto parent_gate = std::make_shared<Gate>();
    auto live_gate = std::make_shared<Gate>();
    // The frozen `audit.own-child` uses an already-resolved gate: it runs and
    // completes on its own, so the live parent's held outcome drains.
    auto child_gate = std::make_shared<Gate>();
    child_gate->release();
    std::vector<TaskDefinition> tasks;
    add_gated_task(tasks, "audit.own-parent", log, parent_gate, "parent");
    add_gated_task(tasks, "audit.own-live", log, live_gate, "live");
    add_gated_task(tasks, "audit.own-child", log, child_gate, "child");

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId parent_id;
    auto parent_commit = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.own-parent"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        parent_id = *created;
        return {};
    }));
    REQUIRE(parent_commit.has_value());
    REQUIRE(harness->resume().has_value());
    parent_gate->release();
    REQUIRE(consume(harness->wait_for_task(parent_id)).has_value());

    ConversationId other_id;
    auto other_commit = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_conversation();
        if (!created) return std::unexpected(created.error());
        other_id = created->id;
        return {};
    }));
    REQUIRE(other_commit.has_value());

    const auto* expected_errors = scenario->object_member("errors");
    REQUIRE(expected_errors != nullptr);

    // A rejected commit reports the frozen ownership message; the frozen id is
    // pi's sequence position, so the C++ side interpolates its own id into
    // the same template (SessionTransactionTest's frozen-template discipline).
    const auto attempt = [&](auto run) -> std::string {
        auto outcome = consume(harness->commit(std::move(run)));
        if (outcome.has_value()) return "accepted";
        return outcome.error().message;
    };

    const std::string frozen_terminal = expected_errors->at("child_of_terminal").get_string();
    const std::string expected_terminal = "Task owner " + std::to_string(parent_id.value) + " is terminal";
    CHECK(frozen_terminal == "Task owner 7 is terminal"); // pin the frozen template
    CHECK(attempt([&](Transaction& tx) -> support::Expected<void> {
        auto created = tx.create_task(
                *harness->task_definition("audit.own-child"), chord::Json{}, TaskOptions{.owner = parent_id});
        if (!created) return std::unexpected(created.error());
        return {};
    }) == expected_terminal);

    CHECK(attempt([&](Transaction& tx) -> support::Expected<void> {
        auto created = tx.create_task(
                *harness->task_definition("audit.own-child"), chord::Json{}, TaskOptions{.owner = TaskId{999999}});
        if (!created) return std::unexpected(created.error());
        return {};
    }) == expected_errors->at("child_of_missing").get_string());

    CHECK(attempt([&](Transaction& tx) -> support::Expected<void> {
        auto created = tx.create_task(*harness->task_definition("audit.own-child"),
                chord::Json{},
                TaskOptions{.owner = parent_id, .background = true});
        if (!created) return std::unexpected(created.error());
        return {};
    }) == expected_errors->at("background_child").get_string());

    CHECK(expected_errors->at("child_other_conversation").get_string() ==
            "A child task lives in its owner's conversation 1"); // pin the frozen template
    const std::string expected_other =
            "A child task lives in its owner's conversation " + std::to_string(root.value().value);
    CHECK(attempt([&](Transaction& tx) -> support::Expected<void> {
        auto created = tx.create_task(*harness->task_definition("audit.own-child"),
                chord::Json{},
                TaskOptions{.conversation_id = other_id, .owner = parent_id});
        if (!created) return std::unexpected(created.error());
        return {};
    }) == expected_other);

    CHECK(expected_errors->at("conversation_of_terminal").get_string() == "Conversation owner task 7 is terminal");
    const std::string expected_conversation_terminal =
            "Conversation owner task " + std::to_string(parent_id.value) + " is terminal";
    CHECK(attempt([&](Transaction& tx) -> support::Expected<void> {
        auto created = tx.create_conversation(parent_id);
        if (!created) return std::unexpected(created.error());
        return {};
    }) == expected_conversation_terminal);

    // A live owner takes a child and a task-owned conversation names it.
    TaskId live_parent;
    auto live_commit = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.own-live"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        live_parent = *created;
        return {};
    }));
    REQUIRE(live_commit.has_value());

    TaskId accepted_child;
    std::optional<ConversationRecord> accepted_conversation;
    auto accept_commit = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto child = tx.create_task(
                *harness->task_definition("audit.own-child"), chord::Json{}, TaskOptions{.owner = live_parent});
        if (!child) return std::unexpected(child.error());
        accepted_child = *child;
        auto conversation = tx.create_conversation(live_parent);
        if (!conversation) return std::unexpected(conversation.error());
        accepted_conversation = *conversation;
        return {};
    }));
    REQUIRE(accept_commit.has_value());

    // The frozen `acceptedChildOwner`/`acceptedConversationOwner` are pi's own
    // task ids; across environments the check is structural: the child names
    // the live owner, and the conversation's creator edge names it too.
    auto accepted = consume(harness->get_task(accepted_child));
    REQUIRE(accepted.has_value());
    REQUIRE(accepted.value().has_value());
    CHECK(accepted.value()->owner == std::optional<TaskId>{live_parent});

    REQUIRE(accepted_conversation.has_value());
    REQUIRE(accepted_conversation->owner.has_value());
    CHECK(accepted_conversation->owner->task_id == live_parent);
    CHECK(accepted_conversation->owner->conversation_id == root.value());
    CHECK(accepted_conversation->id != root.value());
    CHECK(accepted_conversation->id != other_id);
    CHECK(scenario->expected().at("acceptedConversationIdIsFresh").get_boolean());

    live_gate->release();
    REQUIRE(consume(harness->wait_for_task(live_parent)).has_value());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a completing owner holds its terminal outcome until ordinary owned work drains",
        "[durable][ownership][issue1078]") {
    const auto scenario = FrozenScenario::load(
            kTaskLifecycleArtifact, "a completing owner holds its terminal outcome until ordinary owned work drains");
    REQUIRE(scenario.has_value());

    SyncLog log;
    auto child_gate = std::make_shared<Gate>();
    std::vector<TaskDefinition> tasks;
    add_gated_task(tasks, "audit.hold-child", log, child_gate, "child");

    const TaskDefinition* child_def = nullptr;
    {
        auto builder = TaskDefinitionBuilder("audit.hold-parent", 1);
        builder.initial([](const chord::Json&) { return checkpoint_phase("run"); });
        builder.phase("run", [&log, &child_def](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            auto spawned = consume(
                    runtime.commit([&log, child_def, owner = runtime.task_id()](Transaction& tx,
                                           const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        auto created = tx.create_task(*child_def, chord::Json{}, TaskOptions{.owner = owner});
                        if (!created) return std::unexpected(created.error());
                        log.push("parent-commit-completed");
                        return std::optional<NextTaskState>{};
                    }));
            if (!spawned.has_value()) return support::AsyncResult<void>{std::unexpected(spawned.error())};
            return runtime.commit([](Transaction&,
                                          const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("parent")});
            });
        });
        builder.abort([](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            return runtime.commit(
                    [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                        return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
                    });
        });
        auto def = builder.build();
        REQUIRE(def.has_value());
        tasks.push_back(std::move(*def));
    }

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());
    child_def = harness->task_definition("audit.hold-child");
    REQUIRE(child_def != nullptr);
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId parent_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.hold-parent"),
                chord::Json{},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        parent_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());
    REQUIRE(harness->resume().has_value());

    wait_until([&] { return log.includes("parent-commit-completed"); }, "parent completed commit");
    wait_until([&] { return log.includes("audit.hold-child:run"); }, "child running");

    // The held parent: record `completing`, inspection `completing`; the child
    // is still running.
    auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    const auto* expected_before = scenario->array_member("before");
    REQUIRE(expected_before != nullptr);
    REQUIRE(inspection.value().tasks.size() == expected_before->size());
    for (std::size_t i = 0; i < expected_before->size(); ++i) {
        const TaskInspection& task = inspection.value().tasks[i];
        const auto& expected = expected_before->at(i);
        CHECK(task.record.kind == expected.at("kind").get_string());
        CHECK(task.record.state.status == expected.at("recordStatus").get_string());
        CHECK(task.state.kind == expected.at("inspection").get_string());
    }

    child_gate->release();
    auto receipt_res = consume(harness->wait_for_task(parent_id));
    REQUIRE(receipt_res.has_value());
    const auto* expected_parent = scenario->object_member("parentOutcome");
    REQUIRE(expected_parent != nullptr);
    REQUIRE(receipt_res.value().state.outcome.has_value());
    CHECK(receipt_res.value().state.outcome->status == expected_parent->at("status").get_string());
    CHECK(json_equals(*receipt_res.value().state.outcome->result, expected_parent->at("result")));

    auto scanned_children = storage.scan_tasks(16);
    REQUIRE(scanned_children.has_value());
    const TaskRecord* child = nullptr;
    for (const TaskRecord& record : *scanned_children) {
        if (record.kind == "audit.hold-child") child = &record;
    }
    REQUIRE(child != nullptr);
    const auto* expected_child = scenario->object_member("childOutcome");
    REQUIRE(expected_child != nullptr);
    REQUIRE(child->state.outcome.has_value());
    CHECK(child->state.outcome->status == expected_child->at("status").get_string());
    CHECK(json_equals(*child->state.outcome->result, expected_child->at("result")));

    REQUIRE(consume(harness->close()).has_value());
}

} // namespace
} // namespace cch::tests::durable
