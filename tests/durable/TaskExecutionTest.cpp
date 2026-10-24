#include <cch/chord/Json.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include "FrozenExpectation.hpp"
#include "Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

const std::string kAuditArtifact = "fixtures/pi-v1.3.0/audit-1076/task-outcomes.json";

std::filesystem::path unique_temp_dir(std::string_view prefix) {
    const auto p = std::filesystem::temp_directory_path() /
                   (std::string{prefix} + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("TaskExecution: multi-phase counter task advances checkpoints and commits completed receipt",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "a multi-phase task advances checkpoints and commits a completed terminal receipt");
    REQUIRE(scenario.has_value());

    std::vector<double> seen_phases;
    int invocation_count = 0;

    auto def_builder = TaskDefinitionBuilder("audit1076.counter", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "count");
        obj.emplace("n", 0.0);
        return chord::Json(std::move(obj));
    });
    def_builder.phase("count", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        invocation_count++;
        const auto* cp = task.state.checkpoint ? task.state.checkpoint->as_object() : nullptr;
        REQUIRE(cp != nullptr);
        const double current_n = *cp->at("n").as_number();
        seen_phases.push_back(current_n);

        const auto* input_obj = task.input.as_object();
        REQUIRE(input_obj != nullptr);
        const double target = *input_obj->at("to").as_number();

        return runtime.commit([target](Transaction&, const TaskRecord& current)
                                      -> support::Expected<std::optional<NextTaskState>> {
            const auto* cur_cp = current.state.checkpoint ? current.state.checkpoint->as_object() : nullptr;
            if (cur_cp == nullptr) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "No checkpoint"));
            }
            const double next_n = *cur_cp->at("n").as_number() + 1.0;
            if (next_n >= target) {
                return NextTaskState::terminal(TaskOutcome{
                        .status = "completed",
                        .result = chord::Json(next_n),
                });
            }
            chord::Json::Object next_cp;
            next_cp.emplace("phase", "count");
            next_cp.emplace("n", next_n);
            return NextTaskState::running(chord::Json(std::move(next_cp)));
        });
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id;
    chord::Json::Object input;
    input.emplace("to", 3.0);
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto& registered_def = harness->session(); // compile check
        (void)registered_def;
        // Lookup definition from harness
        auto created = tx.create_task(
                *harness->task_definition("audit1076.counter"), chord::Json(input));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());

    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    // Check against frozen observations
    const auto* expected_phases = scenario->array_member("phasesSeen");
    REQUIRE(expected_phases != nullptr);
    REQUIRE(seen_phases.size() == expected_phases->size());
    for (std::size_t i = 0; i < seen_phases.size(); ++i) {
        CHECK(seen_phases[i] == expected_phases->at(i).get_number());
    }

    CHECK(receipt.state.status == *scenario->string_member("status"));
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == *scenario->string_member("outcomeStatus"));
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(*receipt.state.outcome->result->as_number() == *scenario->number_member("result"));

    // started_at and ended_at presence
    CHECK(receipt.started_at.has_value());
    CHECK(receipt.ended_at.has_value());
    // memos absent / empty on terminal
    CHECK(receipt.memos.empty());

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: task memo survives checkpoint replacement and is cleared on terminal settlement",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "a task memo survives checkpoint replacement and is cleared on terminal settlement");
    REQUIRE(scenario.has_value());

    std::map<std::string, chord::Json> running_memos_seen;

    auto def_builder = TaskDefinitionBuilder("audit1076.memo", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "p1");
        return chord::Json(std::move(obj));
    });
    def_builder.phase("p1", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto* input_obj = task.input.as_object();
        REQUIRE(input_obj != nullptr);
        const double amount = *input_obj->at("amount").as_number();

        return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
                [&runtime, amount](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                    runtime.memo("requested", chord::Json(amount))
                            .start([&runtime, completion = std::move(completion)](auto) mutable noexcept {
                                chord::Json::Object p2_cp;
                                p2_cp.emplace("phase", "p2");
                                runtime.commit([p2_cp = std::move(p2_cp)](Transaction&, const TaskRecord&)
                                                       -> support::Expected<std::optional<NextTaskState>> {
                                           return NextTaskState::running(chord::Json(p2_cp));
                                       })
                                        .start(std::move(completion));
                            });
                }}};
    });
    def_builder.phase("p2", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        running_memos_seen = task.memos;
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "completed",
                    .result = chord::Json("done"),
            });
        });
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id;
    chord::Json::Object input;
    input.emplace("amount", 7.0);
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.memo"), chord::Json(input));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());

    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    // Running memo check: "requested" was 7
    REQUIRE(running_memos_seen.contains("requested"));
    CHECK(*running_memos_seen["requested"].as_number() == 7.0);

    // Terminal memo check: memos must be empty on terminal settlement
    CHECK(receipt.memos.empty());
    CHECK(receipt.state.status == *scenario->string_member("terminalStatus"));
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == *scenario->string_member("outcomeStatus"));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: task commits typed failed outcome with error detail and partial result",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "a multi-phase task commits a typed failed outcome with error and partial result");
    REQUIRE(scenario.has_value());

    auto def_builder = TaskDefinitionBuilder("audit1076.fail", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase("run", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        chord::Json::Object detail;
        detail.emplace("code", 402.0);
        chord::Json::Object partial;
        partial.emplace("partial", 42.0);

        return runtime.commit([detail = std::move(detail), partial = std::move(partial)](
                                      Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "failed",
                    .result = chord::Json(partial),
                    .error = TaskOutcomeError{
                            .message = "transfer declined",
                            .detail = chord::Json(detail),
                    },
            });
        });
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.fail"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    CHECK(receipt.state.status == *scenario->string_member("status"));
    REQUIRE(receipt.state.outcome.has_value());
    const auto& outcome = *receipt.state.outcome;
    CHECK(outcome.status == "failed");
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->message == "transfer declined");
    REQUIRE(outcome.error->detail.has_value());
    CHECK(*outcome.error->detail->as_object()->at("code").as_number() == 402.0);
    REQUIRE(outcome.result.has_value());
    CHECK(*outcome.result->as_object()->at("partial").as_number() == 42.0);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: abort_task marks running task and executes abort handler",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "abortTask marks a running task and runs its abort handler to commit an aborted outcome");
    REQUIRE(scenario.has_value());

    bool abort_run_signalled = false;
    bool abort_handler_ran = false;

    auto def_builder = TaskDefinitionBuilder("audit1076.abort", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        // Run until stop is requested
        while (!runtime.stop_token().stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        abort_run_signalled = true;
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    def_builder.abort([&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        abort_handler_ran = true;
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "aborted",
                    .reason = "requested",
            });
        });
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.abort"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto abort_res = consume(harness->abort_task(task_id));
    REQUIRE(abort_res.has_value());
    CHECK(abort_res.value() == *scenario->string_member("abortResult"));

    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();
    CHECK(abort_run_signalled);
    CHECK(abort_handler_ran);
    CHECK(receipt.abort_requested);
    CHECK(receipt.state.status == "terminal");
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == "aborted");
    CHECK(receipt.state.outcome->reason == "requested");

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: aborting task with missing definition commits orphaned outcome",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "aborting a task with a missing definition commits an orphaned outcome");
    REQUIRE(scenario.has_value());

    const auto dir = unique_temp_dir("cch_test_orphan");
    const auto db_path = dir / "durable.sqlite";

    TaskId task_id;
    {
        auto placeholder_def = *TaskDefinitionBuilder("audit1076.orphan", 1)
                                        .initial([](const chord::Json&) {
                                            chord::Json::Object obj;
                                            obj.emplace("phase", "run");
                                            return chord::Json(std::move(obj));
                                        })
                                        .phase("run", [](const TaskRecord&, TaskRuntime&) {
                                            return support::AsyncResult<void>{support::ExpectedVoid{}};
                                        })
                                        .build();
        auto storage = *SqliteStorage::open(db_path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(std::move(placeholder_def));
        auto harness_res = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(tasks)}));
        REQUIRE(harness_res.has_value());
        auto harness = std::move(harness_res.value());
        auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
            auto created = tx.create_task(
                    *TaskDefinitionBuilder("audit1076.orphan", 1).build(), chord::Json(chord::Json::Object{}));
            if (!created) return std::unexpected(created.error());
            task_id = *created;
            return {};
        }));
        REQUIRE(commit_res.has_value());
        REQUIRE(consume(harness->close()).has_value());
    }

    // Reopen without task definition registered
    {
        auto storage = *SqliteStorage::open(db_path);
        auto harness_res = consume(Harness::open(*storage, HarnessOptions{}));
        REQUIRE(harness_res.has_value());
        auto harness = std::move(harness_res.value());
        REQUIRE(harness->resume().has_value());

        auto abort_res = consume(harness->abort_task(task_id));
        REQUIRE(abort_res.has_value());
        CHECK(abort_res.value() == *scenario->string_member("abortResult"));

        auto receipt_res = consume(harness->wait_for_task(task_id));
        REQUIRE(receipt_res.has_value());
        const auto& receipt = receipt_res.value();
        CHECK(receipt.state.status == *scenario->string_member("status"));
        REQUIRE(receipt.state.outcome.has_value());
        CHECK(receipt.state.outcome->status == "orphaned");
        CHECK(receipt.state.outcome->reason == "missing_task");

        REQUIRE(consume(harness->close()).has_value());
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("TaskExecution: phase returning without durable progress faults the task",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "a phase returning without durable progress faults the task");
    REQUIRE(scenario.has_value());

    auto def_builder = TaskDefinitionBuilder("audit1076.no-progress", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase("run", [](const TaskRecord&, TaskRuntime&) -> support::AsyncResult<void> {
        // Return without committing durable progress
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.no-progress"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    CHECK(receipt.state.status == *scenario->string_member("status"));
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == *scenario->string_member("outcomeStatus"));
    REQUIRE(receipt.state.outcome->error.has_value());
    CHECK(receipt.state.outcome->error->message == *scenario->string_member("errorMessage"));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: resuming interrupted task continues from checkpoint without rerunning completed phases",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "resuming an interrupted task continues from its durable checkpoint without rerunning completed phases");
    REQUIRE(scenario.has_value());

    const auto dir = unique_temp_dir("cch_test_resume");
    const auto db_path = dir / "durable.sqlite";

    std::vector<std::string> initial_log;
    TaskId task_id;

    // Run phase 1, which commits checkpoint p2
    {
        auto def_builder = TaskDefinitionBuilder("audit1076.resume", 1);
        def_builder.initial([](const chord::Json&) {
            chord::Json::Object obj;
            obj.emplace("phase", "p1");
            return chord::Json(std::move(obj));
        });
        def_builder.phase("p1", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
            initial_log.push_back("p1");
            chord::Json::Object cp;
            cp.emplace("phase", "p2");
            cp.emplace("n", 1.0);
            return runtime.commit([cp = std::move(cp)](Transaction&, const TaskRecord&)
                                          -> support::Expected<std::optional<NextTaskState>> {
                return NextTaskState::running(chord::Json(cp));
            });
        });
        bool interrupt_first = true;
        def_builder.phase("p2", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
            if (interrupt_first) {
                interrupt_first = false;
                while (!runtime.stop_token().stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            }
            const auto* cp = task.state.checkpoint->as_object();
            const double n = *cp->at("n").as_number();
            initial_log.push_back("p2:" + std::to_string(static_cast<int>(n)));
            return runtime.commit([n](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                return NextTaskState::terminal(TaskOutcome{
                        .status = "completed",
                        .result = chord::Json(n + 10.0),
                });
            });
        });

        auto storage = *SqliteStorage::open(db_path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(*def_builder.build());
        auto harness_res = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(tasks)}));
        REQUIRE(harness_res.has_value());
        auto harness = std::move(harness_res.value());

        auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
            auto created = tx.create_task(
                    *harness->task_definition("audit1076.resume"), chord::Json(chord::Json::Object{}));
            if (!created) return std::unexpected(created.error());
            task_id = *created;
            return {};
        }));
        REQUIRE(commit_res.has_value());

        // Resume and let p1 run
        REQUIRE(harness->resume().has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Close harness while p2 is running or before it finishes
        REQUIRE(consume(harness->close()).has_value());
    }

    // Now reopen storage and resume with a definition whose p1 fails the test if called
    std::vector<std::string> reopened_log;
    {
        auto def_builder = TaskDefinitionBuilder("audit1076.resume", 1);
        def_builder.initial([](const chord::Json&) {
            chord::Json::Object obj;
            obj.emplace("phase", "p1");
            return chord::Json(std::move(obj));
        });
        def_builder.phase("p1", [&](const TaskRecord&, TaskRuntime&) -> support::AsyncResult<void> {
            reopened_log.push_back("p1-unexpected");
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Session, "p1 should not rerun"))};
        });
        def_builder.phase("p2", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
            const auto* cp = task.state.checkpoint->as_object();
            const double n = *cp->at("n").as_number();
            reopened_log.push_back("p2:" + std::to_string(static_cast<int>(n)));
            return runtime.commit([n](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                return NextTaskState::terminal(TaskOutcome{
                        .status = "completed",
                        .result = chord::Json(n + 10.0),
                });
            });
        });

        auto storage = *SqliteStorage::open(db_path);
        std::vector<TaskDefinition> tasks;
        tasks.push_back(*def_builder.build());
        auto harness_res = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(tasks)}));
        REQUIRE(harness_res.has_value());
        auto harness = std::move(harness_res.value());

        REQUIRE(consume(harness->open_tasks()).has_value());
        REQUIRE(harness->resume().has_value());

        auto receipt_res = consume(harness->wait_for_task(task_id));
        REQUIRE(receipt_res.has_value());
        const auto& receipt = receipt_res.value();
        CHECK(receipt.state.status == *scenario->string_member("status"));
        REQUIRE(receipt.state.outcome.has_value());
        CHECK(receipt.state.outcome->status == "completed");
        CHECK(*receipt.state.outcome->result->as_number() == *scenario->number_member("result"));

        // Discriminating check: p1 must NOT have run after reopen!
        REQUIRE(reopened_log.size() == 1);
        CHECK(reopened_log[0] == "p2:1");

        REQUIRE(consume(harness->close()).has_value());
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("TaskExecution: terminal record and receipt are committed in the same atomic commit",
          "[durable][task][audit-1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "terminal record and result receipt commit in the same atomic storage batch");
    REQUIRE(scenario.has_value());

    std::vector<CommitPublication> publications;

    auto def_builder = TaskDefinitionBuilder("audit1076.atomic", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase("run", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        chord::Json::Object res;
        res.emplace("receiptId", 999.0);
        return runtime.commit([res = std::move(res)](Transaction&, const TaskRecord&)
                                      -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "completed",
                    .result = chord::Json(res),
            });
        });
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    MemoryStorage storage;
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    REQUIRE(harness->session().subscribe_commits([&](const CommitPublication& pub) {
        publications.push_back(pub);
    }).has_value());

    TaskId task_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.atomic"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    REQUIRE(receipt.state.status == "terminal");
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == "completed");
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(*receipt.state.outcome->result->as_object()->at("receiptId").as_number() == 999.0);

    // Discriminating check: find all commit publications containing the task becoming terminal
    int terminal_commit_count = 0;
    bool found_terminal_with_outcome = false;

    for (const auto& pub : publications) {
        for (const auto& change : pub.changes) {
            if (const auto* table_change = std::get_if<TableCommitChange>(&change)) {
                if (table_change->type == "task" && table_change->task.has_value() &&
                    table_change->task->id == task_id && table_change->task->state.status == "terminal") {
                    terminal_commit_count++;
                    if (table_change->task->state.outcome.has_value() &&
                        table_change->task->state.outcome->status == "completed" &&
                        table_change->task->state.outcome->result.has_value()) {
                        found_terminal_with_outcome = true;
                    }
                }
            }
        }
    }

    // Must be exactly 1 atomic commit that transitions the task to terminal AND carries the receipt/result
    CHECK(terminal_commit_count == 1);
    CHECK(found_terminal_with_outcome);

    REQUIRE(consume(harness->close()).has_value());
}

} // namespace cch::tests::durable
