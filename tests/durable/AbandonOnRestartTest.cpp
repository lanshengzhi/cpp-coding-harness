// Durable `TaskOptions::abandon_on_restart` consumption (ticket #1175).
//
// Every expectation below is read from the frozen pi v1.3.0 artifact
// `fixtures/pi-v1.3.0/audit-1175/abandon-on-restart.json`, captured from the
// named baseline revision by `capture-1175.mts`. The C++ side must reproduce the
// frozen Runtime's own observations: an open-only Harness writes no mark, the
// first later reservation pass marks the task `restart`, that reason reaches the
// abort handler and ordinary owned work, background and unflagged work are not
// reached, a missing definition waits instead of orphaning, and an explicit
// abort request replaces the reason down the tree.

#include <cch/chord/Json.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include <cch/support/JsonValue.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

const std::string kArtifact = "fixtures/pi-v1.3.0/audit-1175/abandon-on-restart.json";

/// A thread-safe append log mirroring the frozen `log` arrays.
struct EventLog {
    mutable std::mutex mutex;
    std::vector<std::string> entries;

    void push(std::string entry) {
        std::lock_guard lock(mutex);
        entries.push_back(std::move(entry));
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        std::lock_guard lock(mutex);
        return entries;
    }

    [[nodiscard]] bool includes(const std::string& entry) const {
        std::lock_guard lock(mutex);
        for (const std::string& existing : entries) {
            if (existing == entry) return true;
        }
        return false;
    }
};

/// The shared abort handler: record the reason the durable record carries and
/// end `aborted` with it (or `requested` when the record carries none).
AbortHandler abort_handler(std::shared_ptr<EventLog> log, std::string kind) {
    return [log, kind](const TaskRecord& task, TaskRuntime& runtime) {
        std::string reason = task.abort_reason.value_or("requested");
        log->push(kind + ":abort:" + reason);
        return runtime.commit([reason = std::move(reason)](Transaction&,
                                      const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "aborted",
                    .reason = std::move(reason),
            });
        });
    };
}

/// A task whose run phase completes with `ran`; its abort handler records the
/// reason it observed and ends `aborted` with that reason (or `requested`).
TaskDefinition role_task(std::string kind, std::shared_ptr<EventLog> log) {
    TaskDefinitionBuilder builder(kind);
    builder.initial([](const chord::Json&) {
        chord::Json::Object checkpoint;
        checkpoint.emplace("phase", "run");
        return chord::Json(std::move(checkpoint));
    });
    builder.phase("run", [log, kind](const TaskRecord&, TaskRuntime& runtime) {
        log->push(kind + ":run");
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "completed",
                    .result = chord::Json(std::string{"ran"}),
            });
        });
    });
    builder.abort(abort_handler(std::move(log), std::move(kind)));
    return std::move(*builder.build());
}

/// Park the current phase until the invocation is stopped (frozen
/// `await untilAborted(runtime.signal)`).
void park_until_stopped(const std::stop_token& token) {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    static_cast<void>(cv.wait(lock, token, [] { return false; }));
}

/// A task whose run phase stays live until its abort signal, then cleans up.
/// The unflagged owner in the request-replaces-restart scenario must not settle
/// before the test requests its abort.
TaskDefinition dormant_task(std::string kind, std::shared_ptr<EventLog> log) {
    TaskDefinitionBuilder builder(kind);
    builder.initial([](const chord::Json&) {
        chord::Json::Object checkpoint;
        checkpoint.emplace("phase", "run");
        return chord::Json(std::move(checkpoint));
    });
    builder.phase("run", [log, kind](const TaskRecord&, TaskRuntime& runtime) {
        log->push(kind + ":run");
        park_until_stopped(runtime.stop_token());
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    builder.abort(abort_handler(std::move(log), std::move(kind)));
    return std::move(*builder.build());
}

[[nodiscard]] std::unique_ptr<Harness> open(std::vector<TaskDefinition> tasks, Storage& storage) {
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(opened.has_value());
    return std::move(opened.value());
}

[[nodiscard]] std::optional<TaskRecord> current_task(Harness& harness, TaskId id) {
    auto read = consume(harness.get_task(id));
    REQUIRE(read.has_value());
    return read.value();
}

[[nodiscard]] TaskRecord require_task(Harness& harness, TaskId id) {
    auto record = current_task(harness, id);
    REQUIRE(record.has_value());
    return *record;
}

/// Poll until `predicate` holds on the committed record, or fail the test.
template <typename Predicate>
[[nodiscard]] TaskRecord wait_for_record(Harness& harness, TaskId id, Predicate predicate, const std::string& what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        auto record = current_task(harness, id);
        REQUIRE(record.has_value());
        if (predicate(*record)) return *record;
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("timed out waiting for " << what);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

[[nodiscard]] chord::Json optional_string(const std::optional<std::string>& value) {
    return value.has_value() ? chord::Json(*value) : chord::Json(nullptr);
}

/// The stable projection of one task record the frozen artifact records.
[[nodiscard]] chord::Json projection(const TaskRecord& record) {
    chord::Json::Object object;
    object.emplace("status", chord::Json(record.state.status));
    object.emplace("abandonOnRestart", chord::Json(record.abandon_on_restart));
    object.emplace("abortRequested", chord::Json(record.abort_requested));
    object.emplace("abortReason", optional_string(record.abort_reason));
    std::optional<std::string> checkpoint_phase;
    if (record.state.checkpoint.has_value()) {
        if (const auto* checkpoint = record.state.checkpoint->as_object(); checkpoint != nullptr) {
            const auto phase = checkpoint->find("phase");
            if (phase != checkpoint->end() && phase->second.as_string() != nullptr) {
                checkpoint_phase = *phase->second.as_string();
            }
        }
    }
    // The frozen `TaskState` union drops the checkpoint on `terminal`, while the
    // durable C++ record has always retained the last one. That shape difference
    // predates this ticket and is orthogonal to the restart mark, so the
    // comparison projects the frozen contract's live-state checkpoint.
    if (record.state.status == "terminal") checkpoint_phase = std::nullopt;
    object.emplace("checkpointPhase", optional_string(checkpoint_phase));
    object.emplace("outcomeStatus",
            record.state.outcome.has_value() ? chord::Json(record.state.outcome->status) : chord::Json(nullptr));
    object.emplace("outcomeReason",
            record.state.outcome.has_value() ? optional_string(record.state.outcome->reason) : chord::Json(nullptr));
    return chord::Json(std::move(object));
}

void check_projection(const TaskRecord& record, const support::JsonValue::object_t& expected, std::string_view label) {
    INFO(std::string{label});
    const chord::Json actual = projection(record);
    const auto actual_text = support::canonical_json(actual.to_support());
    const auto expected_text = support::canonical_json(expected);
    INFO("actual   " << actual_text.value_or("<invalid>"));
    INFO("expected " << expected_text.value_or("<invalid>"));
    CHECK(json_equals(actual, expected));
}

[[nodiscard]] const support::JsonValue::object_t* expected_object(
        const support::JsonValue::object_t& parent, std::string_view key) {
    const auto found = parent.find(key);
    if (found == parent.end()) return nullptr;
    return found->second.get_if<support::JsonValue::object_t>();
}

[[nodiscard]] std::vector<std::string> expected_strings(
        const support::JsonValue::object_t& parent, std::string_view key) {
    std::vector<std::string> values;
    const auto found = parent.find(key);
    REQUIRE(found != parent.end());
    const auto* array = found->second.get_if<support::JsonValue::array_t>();
    REQUIRE(array != nullptr);
    for (const support::JsonValue& item : *array) {
        const auto* text = item.get_if<std::string>();
        REQUIRE(text != nullptr);
        values.push_back(*text);
    }
    return values;
}

struct CreatedTask {
    TaskId id{};
    TaskRecord record{};
};

/// Create one task in its own commit and return its first committed record.
CreatedTask create_task(Harness& harness, std::string_view kind, TaskOptions options) {
    const TaskDefinition* definition = harness.task_definition(kind);
    REQUIRE(definition != nullptr);
    TaskId id;
    auto committed = consume(harness.session().commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*definition, chord::Json(nullptr), options);
        if (!created) return std::unexpected(created.error());
        id = *created;
        return {};
    }));
    REQUIRE(committed.has_value());
    return CreatedTask{id, require_task(harness, id)};
}

struct SettledObservation {
    std::string scheduling;
    TaskRecord after_open{};
    TaskRecord after_open_wait{};
    TaskRecord settlement{};
    TaskRecord settlement_repeat{};
};

/// Observe a later Harness while it stays paused, then resume it and settle
/// the task. The paused observation is taken twice, with a short wait between,
/// so a scheduler that marks at open instead of on the first reservation pass
/// is discriminated.
SettledObservation observe_and_settle(Harness& harness, TaskId id) {
    auto inspection = consume(harness.inspect());
    REQUIRE(inspection.has_value());
    SettledObservation observation;
    observation.scheduling = inspection.value().scheduling;
    observation.after_open = require_task(harness, id);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    observation.after_open_wait = require_task(harness, id);
    REQUIRE(harness.resume().has_value());
    auto receipt = consume(harness.wait_for_task(id));
    REQUIRE(receipt.has_value());
    observation.settlement = receipt.value();
    // A second wait on a terminal task returns the same settled receipt and
    // never reruns the abort handler (no duplicate settlement).
    auto repeat = consume(harness.wait_for_task(id));
    REQUIRE(repeat.has_value());
    observation.settlement_repeat = repeat.value();
    return observation;
}

} // namespace

TEST_CASE("AbandonOnRestart: open-only state and the first reservation pass across memory and SQLite reopen",
        "[durable][restart][issue1175]") {
    const auto scenario = FrozenScenario::load(kArtifact,
            "a flagged task stays unmarked while the next Harness is paused and is marked restart by its first "
            "reservation pass");
    REQUIRE(scenario.has_value());
    const auto& expected = scenario->expected();
    const auto* expected_object_root = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(expected_object_root != nullptr);
    const auto* expected_created = expected_object(*expected_object_root, "created");
    const auto* expected_after_open = expected_object(*expected_object_root, "afterOpen");
    const auto* expected_after_open_wait = expected_object(*expected_object_root, "afterOpenWithoutScheduling");
    const auto* expected_settlement = expected_object(*expected_object_root, "settlement");
    REQUIRE(expected_created != nullptr);
    REQUIRE(expected_after_open != nullptr);
    REQUIRE(expected_after_open_wait != nullptr);
    REQUIRE(expected_settlement != nullptr);
    const auto expected_log = expected_strings(*expected_object_root, "log");
    const auto expected_scheduling = scenario->string_member("schedulingAfterOpen");
    REQUIRE(expected_scheduling.has_value());

    SECTION("in-memory storage identity") {
        const auto log = std::make_shared<EventLog>();
        MemoryStorage storage;
        {
            std::vector<TaskDefinition> tasks;
            tasks.push_back(role_task("audit1175.flagged", log));
            auto first = open(std::move(tasks), storage);
            auto root = consume(first->root());
            REQUIRE(root.has_value());
            const CreatedTask created = create_task(*first,
                    "audit1175.flagged",
                    TaskOptions{.conversation_id = root.value(), .abandon_on_restart = true});
            check_projection(created.record, *expected_created, "created");

            // A later Harness over the same live records. The first Harness is
            // left open because closing it would close the shared memory
            // storage; it never resumes, so it starts nothing.
            std::vector<TaskDefinition> reopened_tasks;
            reopened_tasks.push_back(role_task("audit1175.flagged", log));
            auto second = open(std::move(reopened_tasks), storage);
            const SettledObservation observed = observe_and_settle(*second, created.id);
            CHECK(observed.scheduling == *expected_scheduling);
            check_projection(observed.after_open, *expected_after_open, "afterOpen");
            check_projection(observed.after_open_wait, *expected_after_open_wait, "afterOpenWithoutScheduling");
            check_projection(observed.settlement, *expected_settlement, "settlement");
            check_projection(observed.settlement_repeat, *expected_settlement, "repeated settlement");
            CHECK(log->snapshot() == expected_log);
            REQUIRE(consume(second->close()).has_value());
        }
    }

    SECTION("SQLite close and reopen") {
        const auto log = std::make_shared<EventLog>();
        cch::tests::TempWorkspace workspace;
        const auto path = workspace.path() / "abandon.sqlite";
        TaskId id;
        {
            auto storage = *SqliteStorage::open(path);
            std::vector<TaskDefinition> tasks;
            tasks.push_back(role_task("audit1175.flagged", log));
            auto first = open(std::move(tasks), *storage);
            auto root = consume(first->root());
            REQUIRE(root.has_value());
            const CreatedTask created = create_task(*first,
                    "audit1175.flagged",
                    TaskOptions{.conversation_id = root.value(), .abandon_on_restart = true});
            id = created.id;
            check_projection(created.record, *expected_created, "created");
            REQUIRE(consume(first->close()).has_value());
        }
        auto storage = *SqliteStorage::open(path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(role_task("audit1175.flagged", log));
        auto second = open(std::move(tasks), *storage);
        const SettledObservation observed = observe_and_settle(*second, id);
        CHECK(observed.scheduling == *expected_scheduling);
        check_projection(observed.after_open, *expected_after_open, "afterOpen");
        check_projection(observed.after_open_wait, *expected_after_open_wait, "afterOpenWithoutScheduling");
        check_projection(observed.settlement, *expected_settlement, "settlement");
        check_projection(observed.settlement_repeat, *expected_settlement, "repeated settlement");
        CHECK(log->snapshot() == expected_log);
        REQUIRE(consume(second->close()).has_value());
    }
}

TEST_CASE("AbandonOnRestart: the restart reason cascades to ordinary owned work and stops at background and unflagged "
          "work",
        "[durable][restart][issue1175]") {
    const auto scenario = FrozenScenario::load(kArtifact,
            "a restart mark cascades to ordinary owned work with the same reason and stops at background and "
            "unflagged work");
    REQUIRE(scenario.has_value());
    const auto& expected = scenario->expected();
    const auto* expected_root = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(expected_root != nullptr);
    const auto* expected_after_open = expected_object(*expected_root, "afterOpen");
    const auto* expected_settlement = expected_object(*expected_root, "settlement");
    REQUIRE(expected_after_open != nullptr);
    REQUIRE(expected_settlement != nullptr);
    const auto* expected_parent_open = expected_object(*expected_after_open, "parent");
    const auto* expected_child_open = expected_object(*expected_after_open, "child");
    const auto* expected_background_open = expected_object(*expected_after_open, "background");
    const auto* expected_sibling_open = expected_object(*expected_after_open, "sibling");
    REQUIRE(expected_parent_open != nullptr);
    REQUIRE(expected_child_open != nullptr);
    REQUIRE(expected_background_open != nullptr);
    REQUIRE(expected_sibling_open != nullptr);
    const auto* expected_parent = expected_object(*expected_settlement, "parent");
    const auto* expected_child = expected_object(*expected_settlement, "child");
    const auto* expected_background = expected_object(*expected_settlement, "background");
    const auto* expected_sibling = expected_object(*expected_settlement, "sibling");
    REQUIRE(expected_parent != nullptr);
    REQUIRE(expected_child != nullptr);
    REQUIRE(expected_background != nullptr);
    REQUIRE(expected_sibling != nullptr);
    const auto expected_abort_log = expected_strings(*expected_root, "abortLog");
    const auto expected_run_log = expected_strings(*expected_root, "runLog");

    const auto events = std::make_shared<EventLog>();
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "cascade.sqlite";
    TaskId parent;
    TaskId child;
    TaskId background;
    TaskId sibling;
    {
        auto storage = *SqliteStorage::open(path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(role_task("audit1175.parent", events));
        tasks.push_back(role_task("audit1175.child", events));
        tasks.push_back(role_task("audit1175.background", events));
        tasks.push_back(role_task("audit1175.sibling", events));
        auto first = open(std::move(tasks), *storage);
        auto root = consume(first->root());
        REQUIRE(root.has_value());
        const CreatedTask created_parent = create_task(
                *first, "audit1175.parent", TaskOptions{.conversation_id = root.value(), .abandon_on_restart = true});
        parent = created_parent.id;
        const CreatedTask created_child = create_task(*first, "audit1175.child", TaskOptions{.owner = parent});
        child = created_child.id;
        const TaskDefinition* background_definition = first->task_definition("audit1175.background");
        REQUIRE(background_definition != nullptr);
        auto committed = consume(first->session().commit([&](Transaction& tx) -> support::ExpectedVoid {
            auto conversation = tx.create_conversation(parent);
            if (!conversation) return std::unexpected(conversation.error());
            auto created = tx.create_task(*background_definition,
                    chord::Json(nullptr),
                    TaskOptions{.conversation_id = conversation->id, .background = true});
            if (!created) return std::unexpected(created.error());
            background = *created;
            return {};
        }));
        REQUIRE(committed.has_value());
        const CreatedTask created_sibling =
                create_task(*first, "audit1175.sibling", TaskOptions{.conversation_id = root.value()});
        sibling = created_sibling.id;
        REQUIRE(consume(first->close()).has_value());
    }

    auto storage = *SqliteStorage::open(path);
    std::vector<TaskDefinition> tasks;
    tasks.push_back(role_task("audit1175.parent", events));
    tasks.push_back(role_task("audit1175.child", events));
    tasks.push_back(role_task("audit1175.background", events));
    tasks.push_back(role_task("audit1175.sibling", events));
    auto harness = open(std::move(tasks), *storage);
    check_projection(require_task(*harness, parent), *expected_parent_open, "parent afterOpen");
    check_projection(require_task(*harness, child), *expected_child_open, "child afterOpen");
    check_projection(require_task(*harness, background), *expected_background_open, "background afterOpen");
    check_projection(require_task(*harness, sibling), *expected_sibling_open, "sibling afterOpen");
    REQUIRE(harness->resume().has_value());
    auto parent_receipt = consume(harness->wait_for_task(parent));
    auto child_receipt = consume(harness->wait_for_task(child));
    auto background_receipt = consume(harness->wait_for_task(background));
    auto sibling_receipt = consume(harness->wait_for_task(sibling));
    REQUIRE(parent_receipt.has_value());
    REQUIRE(child_receipt.has_value());
    REQUIRE(background_receipt.has_value());
    REQUIRE(sibling_receipt.has_value());
    check_projection(parent_receipt.value(), *expected_parent, "parent settlement");
    check_projection(child_receipt.value(), *expected_child, "child settlement");
    check_projection(background_receipt.value(), *expected_background, "background settlement");
    check_projection(sibling_receipt.value(), *expected_sibling, "sibling settlement");
    auto entries = events->snapshot();
    std::vector<std::string> abort_log;
    std::vector<std::string> run_log;
    for (const std::string& entry : entries) {
        if (entry.find(":abort:") != std::string::npos) abort_log.push_back(entry);
        if (entry.find(":run") != std::string::npos) run_log.push_back(entry);
    }
    std::ranges::sort(run_log);
    CHECK(abort_log == expected_abort_log);
    CHECK(run_log == expected_run_log);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("AbandonOnRestart: a missing definition waits, an explicit abort orphans it, and a restored definition runs "
          "its abort",
        "[durable][restart][issue1175]") {
    const auto scenario = FrozenScenario::load(kArtifact,
            "a restart-marked task with a missing definition waits; an explicit abort orphans it; a restored "
            "definition runs its abort with reason restart");
    REQUIRE(scenario.has_value());
    const auto& expected = scenario->expected();
    const auto* expected_root = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(expected_root != nullptr);
    const auto* expected_waiting = expected_object(*expected_root, "waitingBeforeAbort");
    REQUIRE(expected_waiting != nullptr);
    const auto* expected_missing_waiting = expected_object(*expected_waiting, "missing");
    const auto* expected_returns_waiting = expected_object(*expected_waiting, "returns");
    const auto* expected_missing_inspection = expected_object(*expected_waiting, "missingInspection");
    REQUIRE(expected_missing_waiting != nullptr);
    REQUIRE(expected_returns_waiting != nullptr);
    REQUIRE(expected_missing_inspection != nullptr);
    const auto* expected_missing_receipt = expected_object(*expected_root, "missingReceipt");
    const auto* expected_returns_receipt = expected_object(*expected_root, "returnsReceipt");
    REQUIRE(expected_missing_receipt != nullptr);
    REQUIRE(expected_returns_receipt != nullptr);
    const auto expected_abort_result = scenario->string_member("abortResult");
    REQUIRE(expected_abort_result.has_value());
    const auto expected_log = expected_strings(*expected_root, "log");

    const auto events = std::make_shared<EventLog>();
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "missing.sqlite";
    TaskId missing;
    TaskId returns;
    {
        auto storage = *SqliteStorage::open(path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(role_task("audit1175.missing", events));
        tasks.push_back(role_task("audit1175.returns", events));
        auto first = open(std::move(tasks), *storage);
        auto root = consume(first->root());
        REQUIRE(root.has_value());
        missing = create_task(
                *first, "audit1175.missing", TaskOptions{.conversation_id = root.value(), .abandon_on_restart = true})
                          .id;
        returns = create_task(
                *first, "audit1175.returns", TaskOptions{.conversation_id = root.value(), .abandon_on_restart = true})
                          .id;
        REQUIRE(consume(first->close()).has_value());
    }

    Registry registry;
    auto storage = *SqliteStorage::open(path);
    auto opened = consume(Harness::open(*storage, HarnessOptions{.registry = &registry}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    REQUIRE(harness->resume().has_value());
    const TaskRecord missing_waiting = wait_for_record(
            *harness,
            missing,
            [](const TaskRecord& record) { return record.abort_reason == "restart"; },
            "the missing-definition task is restart-marked");
    const TaskRecord returns_waiting = wait_for_record(
            *harness,
            returns,
            [](const TaskRecord& record) { return record.abort_reason == "restart"; },
            "the second missing-definition task is restart-marked");
    check_projection(missing_waiting, *expected_missing_waiting, "missing waiting");
    check_projection(returns_waiting, *expected_returns_waiting, "returns waiting");
    auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    std::optional<TaskInspectionState> missing_state;
    for (const TaskInspection& task : inspection.value().tasks) {
        if (task.record.id == missing) missing_state = task.state;
    }
    REQUIRE(missing_state.has_value());
    CHECK(missing_state->kind == *expected_missing_inspection->at("kind").get_if<std::string>());
    REQUIRE(missing_state->reason.has_value());
    CHECK(missing_state->reason == *expected_missing_inspection->at("reason").get_if<std::string>());

    auto abort_result = consume(harness->abort_task(missing));
    REQUIRE(abort_result.has_value());
    CHECK(abort_result.value() == *expected_abort_result);
    auto missing_receipt = consume(harness->wait_for_task(missing));
    REQUIRE(missing_receipt.has_value());
    check_projection(missing_receipt.value(), *expected_missing_receipt, "missing receipt");

    std::vector<TaskDefinition> restored;
    restored.push_back(role_task("audit1175.returns", events));
    Extension extension{.name = "audit1175.returns", .tasks = std::move(restored)};
    REQUIRE(registry.install(std::move(extension)).has_value());
    auto returns_receipt = consume(harness->wait_for_task(returns));
    REQUIRE(returns_receipt.has_value());
    check_projection(returns_receipt.value(), *expected_returns_receipt, "returns receipt");
    CHECK(events->snapshot() == expected_log);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("AbandonOnRestart: an explicit abort above a restart-marked subtree replaces the reason and orphans a "
          "blocked leaf",
        "[durable][restart][issue1175]") {
    const auto scenario = FrozenScenario::load(kArtifact,
            "an explicit abort above a restart-marked subtree replaces the reason and orphans a blocked leaf");
    REQUIRE(scenario.has_value());
    const auto& expected = scenario->expected();
    const auto* expected_root = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(expected_root != nullptr);
    const auto* expected_before = expected_object(*expected_root, "beforeRequest");
    REQUIRE(expected_before != nullptr);
    const auto* expected_top_before = expected_object(*expected_before, "top");
    const auto* expected_middle_before = expected_object(*expected_before, "middle");
    const auto* expected_leaf_before = expected_object(*expected_before, "leaf");
    REQUIRE(expected_top_before != nullptr);
    REQUIRE(expected_middle_before != nullptr);
    REQUIRE(expected_leaf_before != nullptr);
    const auto* expected_settlement = expected_object(*expected_root, "settlement");
    REQUIRE(expected_settlement != nullptr);
    const auto* expected_leaf = expected_object(*expected_settlement, "leaf");
    const auto* expected_middle = expected_object(*expected_settlement, "middle");
    const auto* expected_top = expected_object(*expected_settlement, "top");
    REQUIRE(expected_leaf != nullptr);
    REQUIRE(expected_middle != nullptr);
    REQUIRE(expected_top != nullptr);
    const auto expected_abort_result = scenario->string_member("abortResult");
    REQUIRE(expected_abort_result.has_value());
    const auto expected_log = expected_strings(*expected_root, "log");

    const auto events = std::make_shared<EventLog>();
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "upgrade.sqlite";
    TaskId top;
    TaskId middle;
    TaskId leaf;
    {
        auto storage = *SqliteStorage::open(path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(dormant_task("audit1175.top", events));
        tasks.push_back(role_task("audit1175.middle", events));
        tasks.push_back(role_task("audit1175.leaf", events));
        auto first = open(std::move(tasks), *storage);
        auto root = consume(first->root());
        REQUIRE(root.has_value());
        top = create_task(*first, "audit1175.top", TaskOptions{.conversation_id = root.value()}).id;
        middle = create_task(*first, "audit1175.middle", TaskOptions{.owner = top, .abandon_on_restart = true}).id;
        leaf = create_task(*first, "audit1175.leaf", TaskOptions{.owner = middle}).id;
        REQUIRE(consume(first->close()).has_value());
    }

    // The leaf definition is missing, so it waits under the restart cascade.
    auto storage = *SqliteStorage::open(path);
    std::vector<TaskDefinition> tasks;
    tasks.push_back(dormant_task("audit1175.top", events));
    tasks.push_back(role_task("audit1175.middle", events));
    auto harness = open(std::move(tasks), *storage);
    REQUIRE(harness->resume().has_value());
    const TaskRecord leaf_waiting = wait_for_record(
            *harness,
            leaf,
            [](const TaskRecord& record) {
                return record.abort_reason == "restart" && record.state.status != "terminal";
            },
            "the leaf waits under the restart cascade");
    check_projection(leaf_waiting, *expected_leaf_before, "leaf before request");
    const TaskRecord middle_waiting = wait_for_record(
            *harness,
            middle,
            [](const TaskRecord& record) { return record.abort_reason == "restart"; },
            "the middle task is restart-marked");
    check_projection(middle_waiting, *expected_middle_before, "middle before request");
    const TaskRecord top_running = wait_for_record(
            *harness,
            top,
            [events](const TaskRecord&) { return events->includes("audit1175.top:run"); },
            "the unflagged owner is running");
    check_projection(top_running, *expected_top_before, "top before request");

    auto abort_result = consume(harness->abort_task(top));
    REQUIRE(abort_result.has_value());
    CHECK(abort_result.value() == *expected_abort_result);
    auto leaf_receipt = consume(harness->wait_for_task(leaf));
    auto middle_receipt = consume(harness->wait_for_task(middle));
    auto top_receipt = consume(harness->wait_for_task(top));
    REQUIRE(leaf_receipt.has_value());
    REQUIRE(middle_receipt.has_value());
    REQUIRE(top_receipt.has_value());
    check_projection(leaf_receipt.value(), *expected_leaf, "leaf settlement");
    check_projection(middle_receipt.value(), *expected_middle, "middle settlement");
    check_projection(top_receipt.value(), *expected_top, "top settlement");
    CHECK(events->snapshot() == expected_log);
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("AbandonOnRestart: a flagged task created after open runs instead of being abandoned",
        "[durable][restart][issue1175]") {
    const auto scenario = FrozenScenario::load(
            kArtifact, "a flagged task created after the Harness opened runs instead of being abandoned");
    REQUIRE(scenario.has_value());
    const auto& expected = scenario->expected();
    const auto* expected_root = expected.get_if<support::JsonValue::object_t>();
    REQUIRE(expected_root != nullptr);
    const auto* expected_created = expected_object(*expected_root, "created");
    const auto* expected_settlement = expected_object(*expected_root, "settlement");
    REQUIRE(expected_created != nullptr);
    REQUIRE(expected_settlement != nullptr);
    const auto expected_log = expected_strings(*expected_root, "log");

    const auto log = std::make_shared<EventLog>();
    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(role_task("audit1175.late", log));
    auto harness = open(std::move(tasks), storage);
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    const CreatedTask created = create_task(
            *harness, "audit1175.late", TaskOptions{.conversation_id = root.value(), .abandon_on_restart = true});
    check_projection(created.record, *expected_created, "created");
    REQUIRE(harness->resume().has_value());
    auto receipt = consume(harness->wait_for_task(created.id));
    REQUIRE(receipt.has_value());
    check_projection(receipt.value(), *expected_settlement, "settlement");
    CHECK(log->snapshot() == expected_log);
    REQUIRE(consume(harness->close()).has_value());
}
