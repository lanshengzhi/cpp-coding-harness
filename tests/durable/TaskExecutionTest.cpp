#include <cch/chord/Json.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;
using cch::tests::TempWorkspace;

namespace {

const std::string kAuditArtifact = "fixtures/pi-v1.3.0/audit-1076/task-outcomes.json";

/// Record the exact batches passed to Storage::commit while delegating every
/// operation to the selected real backend.
class CommitObservingStorage final : public Storage {
public:
    explicit CommitObservingStorage(Storage& inner) : inner_(inner) {}

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        commit_batches_.emplace_back(writes.begin(), writes.end());
        return inner_.commit(writes);
    }

    [[nodiscard]] support::Expected<std::int64_t> mint_id() override { return inner_.mint_id(); }

    [[nodiscard]] support::Expected<std::optional<ConversationRecord>> conversation(ConversationId id) override {
        return inner_.conversation(id);
    }

    [[nodiscard]] support::Expected<std::optional<StoredEntry>> entry(EntryId id) override { return inner_.entry(id); }

    [[nodiscard]] support::Expected<std::vector<ConversationRecord>> scan_conversations(std::size_t limit) override {
        return inner_.scan_conversations(limit);
    }

    [[nodiscard]] support::Expected<std::vector<EntryRecord>> scan_entries(
            ConversationId conversation_id, std::size_t limit) override {
        return inner_.scan_entries(conversation_id, limit);
    }

    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, std::optional<ConversationId> conversation_id) override {
        return inner_.find_document(kind, conversation_id);
    }

    [[nodiscard]] support::Expected<std::optional<chord::Json>> document(DocumentId id) override {
        return inner_.document(id);
    }

    [[nodiscard]] support::Expected<std::optional<TaskRecord>> task(TaskId id) override { return inner_.task(id); }

    [[nodiscard]] support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit) override {
        return inner_.scan_tasks(limit);
    }

    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission(SubmissionId id) override {
        return inner_.submission(id);
    }

    [[nodiscard]] support::Expected<std::optional<SubmissionRecord>> submission_by_request(
            ConversationId conversation_id, std::string_view request_id) override {
        return inner_.submission_by_request(conversation_id, request_id);
    }

    [[nodiscard]] support::Expected<std::vector<SubmissionRecord>> scan_submissions(std::size_t limit) override {
        return inner_.scan_submissions(limit);
    }

    [[nodiscard]] support::ExpectedVoid close() override { return inner_.close(); }

    [[nodiscard]] const std::vector<std::vector<StorageWrite>>& commit_batches() const noexcept {
        return commit_batches_;
    }

private:
    Storage& inner_; // must outlive this observing wrapper
    std::vector<std::vector<StorageWrite>> commit_batches_;
};

} // namespace

TEST_CASE("TaskExecution: multi-phase counter task advances checkpoints and commits completed receipt",
        "[durable][task][issue1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "a multi-phase task advances checkpoints and commits a completed terminal receipt");
    REQUIRE(scenario.has_value());

    std::vector<double> seen_phases;
    // Frozen `invocations` counts distinct JS `TaskRuntime` object identities.
    // This harness constructs one `HarnessTaskRuntime` per phase invocation on
    // the stack, so a C++ identity here would be a stack address that may be
    // reused; the observation is recorded as non-comparable instead of being
    // replaced by the phase count (see 1076-task-execution.md).

    auto def_builder = TaskDefinitionBuilder("audit1076.counter", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "count");
        obj.emplace("n", 0.0);
        return chord::Json(std::move(obj));
    });
    // Callback observations: this lambda runs on a Harness execution thread, so
    // it records what it saw and the test thread asserts it after the wait.
    std::atomic_bool phase_checkpoint_missing{false};
    std::atomic_bool phase_input_missing{false};
    def_builder.phase("count", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto* cp = task.state.checkpoint ? task.state.checkpoint->as_object() : nullptr;
        if (cp == nullptr) {
            phase_checkpoint_missing.store(true);
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Validation, "No checkpoint"))};
        }
        const double current_n = *cp->at("n").as_number();
        seen_phases.push_back(current_n);

        const auto* input_obj = task.input.as_object();
        if (input_obj == nullptr) {
            phase_input_missing.store(true);
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Validation, "No input"))};
        }
        const double target = *input_obj->at("to").as_number();

        return runtime.commit(
                [target](Transaction&, const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
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
        auto created = tx.create_task(*harness->task_definition("audit1076.counter"), chord::Json(input));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());

    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    // Callback observations recorded on the execution thread.
    CHECK_FALSE(phase_checkpoint_missing.load());
    CHECK_FALSE(phase_input_missing.load());

    // Check against frozen observations
    const auto* expected_phases = scenario->array_member("phasesSeen");
    REQUIRE(expected_phases != nullptr);
    REQUIRE(seen_phases.size() == expected_phases->size());
    for (std::size_t i = 0; i < seen_phases.size(); ++i) {
        CHECK(seen_phases[i] == expected_phases->at(i).get_number());
    }

    const auto expected_status = scenario->string_member("status");
    REQUIRE(expected_status.has_value());
    CHECK(receipt.state.status == *expected_status);
    REQUIRE(receipt.state.outcome.has_value());
    const auto expected_outcome_status = scenario->string_member("outcomeStatus");
    REQUIRE(expected_outcome_status.has_value());
    CHECK(receipt.state.outcome->status == *expected_outcome_status);
    REQUIRE(receipt.state.outcome->result.has_value());
    const auto expected_result = scenario->number_member("result");
    REQUIRE(expected_result.has_value());
    const auto* result_number = receipt.state.outcome->result->as_number();
    REQUIRE(result_number != nullptr);
    CHECK(*result_number == *expected_result);

    // started_at and ended_at presence
    CHECK(receipt.started_at.has_value());
    CHECK(receipt.ended_at.has_value());
    // memos absent / empty on terminal
    CHECK(receipt.memos.empty());

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: task memo survives checkpoint replacement and is cleared on terminal settlement",
        "[durable][task][issue1076]") {
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
        if (input_obj == nullptr) {
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Validation, "No input"))};
        }
        const double amount = *input_obj->at("amount").as_number();

        // `runtime` is a reference to the Harness-owned runtime for this phase
        // invocation. Its lifetime spans the returned AsyncResult: the Harness
        // constructs it on the execution stack and only destroys it after this
        // result's terminal completion, so capturing the reference here is
        // valid for as long as the producer's callbacks can run.
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
        auto created = tx.create_task(*harness->task_definition("audit1076.memo"), chord::Json(input));
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
    const auto* requested_amount = running_memos_seen["requested"].as_number();
    REQUIRE(requested_amount != nullptr);
    CHECK(*requested_amount == 7.0);

    // Terminal memo check: memos must be empty on terminal settlement
    CHECK(receipt.memos.empty());
    const auto expected_terminal_status = scenario->string_member("terminalStatus");
    REQUIRE(expected_terminal_status.has_value());
    CHECK(receipt.state.status == *expected_terminal_status);
    REQUIRE(receipt.state.outcome.has_value());
    const auto expected_outcome_status = scenario->string_member("outcomeStatus");
    REQUIRE(expected_outcome_status.has_value());
    CHECK(receipt.state.outcome->status == *expected_outcome_status);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: task commits typed failed outcome with error detail and partial result",
        "[durable][task][issue1076]") {
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

        return runtime.commit([detail = std::move(detail), partial = std::move(partial)](Transaction&,
                                      const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "failed",
                    .result = chord::Json(partial),
                    .error =
                            TaskOutcomeError{
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
        auto created = tx.create_task(*harness->task_definition("audit1076.fail"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    const auto expected_status = scenario->string_member("status");
    REQUIRE(expected_status.has_value());
    CHECK(receipt.state.status == *expected_status);
    REQUIRE(receipt.state.outcome.has_value());
    const auto& outcome = *receipt.state.outcome;
    CHECK(outcome.status == "failed");
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->message == "transfer declined");
    REQUIRE(outcome.error->detail.has_value());
    const auto* detail_object = outcome.error->detail->as_object();
    REQUIRE(detail_object != nullptr);
    const auto* code_number = detail_object->at("code").as_number();
    REQUIRE(code_number != nullptr);
    CHECK(*code_number == 402.0);
    REQUIRE(outcome.result.has_value());
    const auto* result_object = outcome.result->as_object();
    REQUIRE(result_object != nullptr);
    const auto* partial_number = result_object->at("partial").as_number();
    REQUIRE(partial_number != nullptr);
    CHECK(*partial_number == 42.0);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: abort_task marks running task and executes abort handler", "[durable][task][issue1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "abortTask marks a running task and runs its abort handler to commit an aborted outcome");
    REQUIRE(scenario.has_value());

    std::atomic_bool phase_memo_rejected{false};
    std::atomic_bool phase_commit_rejected{false};
    std::atomic_bool abort_handler_ran{false};
    std::atomic_bool abort_memo_committed{false};
    std::mutex phase_mutex;
    std::condition_variable phase_cv;
    bool phase_started = false;

    auto def_builder = TaskDefinitionBuilder("audit1076.abort", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const std::stop_token stop = runtime.stop_token();
        {
            std::unique_lock lock(phase_mutex);
            phase_started = true;
            phase_cv.notify_all();
            std::stop_callback wake_on_stop{stop, [&phase_cv] { phase_cv.notify_all(); }};
            if (!phase_cv.wait_for(lock, std::chrono::seconds{30}, [&stop] { return stop.stop_requested(); })) {
                return support::AsyncResult<void>{std::unexpected(
                        support::make_error(support::ErrorCode::Timeout, "Timed out waiting for abort request"))};
            }
        }
        auto memo = consume(runtime.memo("after-abort", chord::Json("late")));
        phase_memo_rejected.store(!memo.has_value());
        auto progress = consume(
                runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::terminal(
                            TaskOutcome{.status = "completed", .result = chord::Json("too late")});
                }));
        phase_commit_rejected.store(!progress.has_value());
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    def_builder.abort([&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        abort_handler_ran.store(true);
        auto memo = consume(runtime.memo("abort-cleanup", chord::Json("saved")));
        abort_memo_committed.store(memo.has_value());
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "requested"});
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
        auto created = tx.create_task(*harness->task_definition("audit1076.abort"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    {
        std::unique_lock lock(phase_mutex);
        REQUIRE(phase_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return phase_started; }));
    }

    auto abort_res = consume(harness->abort_task(task_id));
    REQUIRE(abort_res.has_value());
    const auto expected_abort_result = scenario->string_member("abortResult");
    REQUIRE(expected_abort_result.has_value());
    CHECK(abort_res.value() == *expected_abort_result);
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();
    CHECK(phase_memo_rejected.load());
    CHECK(phase_commit_rejected.load());
    CHECK(abort_handler_ran.load());
    CHECK(abort_memo_committed.load());
    CHECK(receipt.abort_requested);
    CHECK(receipt.state.status == "terminal");
    REQUIRE(receipt.state.outcome.has_value());
    REQUIRE(receipt.state.outcome->reason.has_value());
    const auto* expected_outcome = scenario->object_member("outcome");
    REQUIRE(expected_outcome != nullptr);
    chord::Json::Object actual_outcome;
    actual_outcome.emplace("status", receipt.state.outcome->status);
    actual_outcome.emplace("reason", *receipt.state.outcome->reason);
    CHECK(json_equals(chord::Json(std::move(actual_outcome)), *expected_outcome));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: abort handler may commit a failed terminal outcome", "[durable][task][issue1076]") {
    auto def_builder = TaskDefinitionBuilder("audit1076.abort-failure", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase(
            "run", [](const TaskRecord&, TaskRuntime&) { return support::AsyncResult<void>{support::ExpectedVoid{}}; });
    def_builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "failed",
                    .error = TaskOutcomeError{.message = "cleanup failed"},
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
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.abort-failure"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());
    auto abort_res = consume(harness->abort_task(task_id));
    REQUIRE(abort_res.has_value());
    CHECK(abort_res.value() == "marked");

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    REQUIRE(receipt_res.value().state.outcome.has_value());
    CHECK(receipt_res.value().state.outcome->status == "failed");
    REQUIRE(receipt_res.value().state.outcome->error.has_value());
    CHECK(receipt_res.value().state.outcome->error->message == "cleanup failed");
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "TaskExecution: aborting task with missing definition commits orphaned outcome", "[durable][task][issue1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "aborting a task with a missing definition commits an orphaned outcome");
    REQUIRE(scenario.has_value());

    TempWorkspace workspace;
    const auto db_path = workspace.path() / "durable.sqlite";

    TaskId task_id;
    {
        auto placeholder_def = *TaskDefinitionBuilder("audit1076.orphan", 1)
                                        .initial([](const chord::Json&) {
                                            chord::Json::Object obj;
                                            obj.emplace("phase", "run");
                                            return chord::Json(std::move(obj));
                                        })
                                        .phase("run",
                                                [](const TaskRecord&, TaskRuntime&) {
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
        const auto expected_abort_result = scenario->string_member("abortResult");
        REQUIRE(expected_abort_result.has_value());
        CHECK(abort_res.value() == *expected_abort_result);

        auto receipt_res = consume(harness->wait_for_task(task_id));
        REQUIRE(receipt_res.has_value());
        const auto& receipt = receipt_res.value();
        const auto expected_status = scenario->string_member("status");
        REQUIRE(expected_status.has_value());
        CHECK(receipt.state.status == *expected_status);
        REQUIRE(receipt.state.outcome.has_value());
        CHECK(receipt.state.outcome->status == "orphaned");
        CHECK(receipt.state.outcome->reason == "missing_task");

        REQUIRE(consume(harness->close()).has_value());
    }
}

TEST_CASE("TaskExecution: phase returning without durable progress faults the task", "[durable][task][issue1076]") {
    const auto scenario =
            FrozenScenario::load(kAuditArtifact, "a phase returning without durable progress faults the task");
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
        auto created =
                tx.create_task(*harness->task_definition("audit1076.no-progress"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    }));
    REQUIRE(commit_res.has_value());

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    const auto expected_status = scenario->string_member("status");
    REQUIRE(expected_status.has_value());
    CHECK(receipt.state.status == *expected_status);
    REQUIRE(receipt.state.outcome.has_value());
    const auto expected_outcome_status = scenario->string_member("outcomeStatus");
    REQUIRE(expected_outcome_status.has_value());
    CHECK(receipt.state.outcome->status == *expected_outcome_status);
    REQUIRE(receipt.state.outcome->error.has_value());
    const auto expected_error_message = scenario->string_member("errorMessage");
    REQUIRE(expected_error_message.has_value());
    CHECK(receipt.state.outcome->error->message == *expected_error_message);

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: resuming interrupted task continues from checkpoint without rerunning completed phases",
        "[durable][task][issue1076]") {
    const auto scenario = FrozenScenario::load(kAuditArtifact,
            "resuming an interrupted task continues from its durable checkpoint without rerunning completed phases");
    REQUIRE(scenario.has_value());

    TempWorkspace workspace;
    const auto db_path = workspace.path() / "durable.sqlite";

    std::vector<std::string> initial_log;
    TaskId task_id;
    std::mutex phase_mutex;
    std::condition_variable phase_cv;
    bool p2_started = false;

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
            return runtime.commit([cp = std::move(cp)](Transaction&,
                                          const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                return NextTaskState::running(chord::Json(cp));
            });
        });
        bool interrupt_first = true;
        def_builder.phase("p2", [&](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
            if (interrupt_first) {
                interrupt_first = false;
                const std::stop_token stop = runtime.stop_token();
                std::unique_lock lock(phase_mutex);
                p2_started = true;
                phase_cv.notify_all();
                std::stop_callback wake_on_stop{stop, [&phase_cv] { phase_cv.notify_all(); }};
                if (!phase_cv.wait_for(lock, std::chrono::seconds{30}, [&stop] { return stop.stop_requested(); })) {
                    return support::AsyncResult<void>{std::unexpected(
                            support::make_error(support::ErrorCode::Timeout, "Timed out waiting for harness close"))};
                }
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            }
            const auto* cp = task.state.checkpoint ? task.state.checkpoint->as_object() : nullptr;
            if (cp == nullptr) {
                return support::AsyncResult<void>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "No checkpoint"))};
            }
            const double n = *cp->at("n").as_number();
            initial_log.push_back("p2:" + std::to_string(static_cast<int>(n)));
            return runtime.commit(
                    [n](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
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
            auto created =
                    tx.create_task(*harness->task_definition("audit1076.resume"), chord::Json(chord::Json::Object{}));
            if (!created) return std::unexpected(created.error());
            task_id = *created;
            return {};
        }));
        REQUIRE(commit_res.has_value());

        REQUIRE(harness->resume().has_value());
        {
            std::unique_lock lock(phase_mutex);
            REQUIRE(phase_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return p2_started; }));
        }
        // Closing requests stop after p1 has committed the p2 checkpoint and p2 has started.
        REQUIRE(consume(harness->close()).has_value());

        // Before close, only p1 had recorded its entry: p2's interrupt branch
        // returns before logging. This is the pre-interruption half of the
        // resume observation, asserted here on the test thread after close.
        REQUIRE(initial_log.size() == 1);
        CHECK(initial_log[0] == "p1");
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
            const auto* cp = task.state.checkpoint ? task.state.checkpoint->as_object() : nullptr;
            if (cp == nullptr) {
                return support::AsyncResult<void>{
                        std::unexpected(support::make_error(support::ErrorCode::Validation, "No checkpoint"))};
            }
            const double n = *cp->at("n").as_number();
            reopened_log.push_back("p2:" + std::to_string(static_cast<int>(n)));
            return runtime.commit(
                    [n](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
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
        const auto expected_status = scenario->string_member("status");
        REQUIRE(expected_status.has_value());
        CHECK(receipt.state.status == *expected_status);
        REQUIRE(receipt.state.outcome.has_value());
        CHECK(receipt.state.outcome->status == "completed");
        REQUIRE(receipt.state.outcome->result.has_value());
        const auto* resumed_result = receipt.state.outcome->result->as_number();
        REQUIRE(resumed_result != nullptr);
        const auto expected_result = scenario->number_member("result");
        REQUIRE(expected_result.has_value());
        CHECK(*resumed_result == *expected_result);

        // Discriminating check: p1 must NOT have run after reopen!
        REQUIRE(reopened_log.size() == 1);
        CHECK(reopened_log[0] == "p2:1");

        // The frozen reopen observation is the resumed phase log itself.
        const auto* expected_reopened = scenario->array_member("reopenedPhasesRan");
        REQUIRE(expected_reopened != nullptr);
        REQUIRE(expected_reopened->size() == reopened_log.size());
        for (std::size_t i = 0; i < reopened_log.size(); ++i) {
            CHECK(reopened_log[i] == expected_reopened->at(i).get_string());
        }

        REQUIRE(consume(harness->close()).has_value());
    }
}

TEST_CASE("TaskExecution: terminal record and receipt are committed in the same atomic commit",
        "[durable][task][issue1076]") {
    const auto scenario = FrozenScenario::load(
            kAuditArtifact, "terminal record and result receipt commit in the same atomic storage batch");
    REQUIRE(scenario.has_value());

    TempWorkspace workspace;
    const auto db_path = workspace.path() / "atomic.sqlite";
    auto sqlite_storage = *SqliteStorage::open(db_path);
    CommitObservingStorage storage{*sqlite_storage};

    auto def_builder = TaskDefinitionBuilder("audit1076.atomic", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    chord::Json::Object expected_result;
    expected_result.emplace("receiptId", 999.0);
    const auto expected_json = chord::Json(expected_result);
    def_builder.phase("run", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        chord::Json::Object result;
        result.emplace("receiptId", 999.0);
        return runtime.commit([result = std::move(result)](Transaction&,
                                      const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "completed",
                    .result = chord::Json(result),
            });
        });
    });

    auto def = def_builder.build();
    REQUIRE(def.has_value());

    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*def));
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id;
    auto commit_res = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created =
                tx.create_task(*harness->task_definition("audit1076.atomic"), chord::Json(chord::Json::Object{}));
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
    const auto* receipt_result_object = receipt.state.outcome->result->as_object();
    REQUIRE(receipt_result_object != nullptr);
    const auto* receipt_id_number = receipt_result_object->at("receiptId").as_number();
    REQUIRE(receipt_id_number != nullptr);
    CHECK(*receipt_id_number == 999.0);

    // The receipt is the result inside the terminal TaskRecord. Observe the exact
    // batch passed to Storage::commit and require both in that same task write.
    std::size_t terminal_write_count = 0;
    bool found_receipt_in_terminal_batch = false;
    for (const auto& batch : storage.commit_batches()) {
        for (const StorageWrite& write : batch) {
            if (write.type != "task" || !write.task.has_value() || write.task->id != task_id ||
                    write.task->state.status != "terminal") {
                continue;
            }
            ++terminal_write_count;
            found_receipt_in_terminal_batch =
                    batch.size() == 1 && write.task->state.outcome.has_value() &&
                    write.task->state.outcome->status == "completed" &&
                    write.task->state.outcome->result == std::optional<chord::Json>{expected_json};
        }
    }

    CHECK(terminal_write_count == 1);
    CHECK(found_receipt_in_terminal_batch);

    REQUIRE(consume(harness->close()).has_value());

    auto reopened_storage = *SqliteStorage::open(db_path);
    auto persisted = reopened_storage->task(task_id);
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->has_value());
    CHECK((*persisted)->state.status == "terminal");
    REQUIRE((*persisted)->state.outcome.has_value());
    REQUIRE((*persisted)->state.outcome->result.has_value());
    CHECK(*(*persisted)->state.outcome->result == expected_json);
    REQUIRE(reopened_storage->close().has_value());
}

TEST_CASE("TaskExecution: building a task definition twice reports a validation error", "[durable][task][issue1076]") {
    auto builder = TaskDefinitionBuilder("audit1076.repeat-build", 1);
    builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    builder.phase(
            "run", [](const TaskRecord&, TaskRuntime&) { return support::AsyncResult<void>{support::ExpectedVoid{}}; });

    REQUIRE(builder.valid());
    auto first = builder.build();
    REQUIRE(first.has_value());
    CHECK(first->kind() == "audit1076.repeat-build");
    CHECK(first->version() == 1);
    CHECK(first->has_phase("run"));

    // `build()` consumed the builder: it owns no definition any more.
    CHECK_FALSE(builder.valid());

    // Mutators after a successful build are documented no-ops rather than a
    // null dereference, and the builder stays consumed.
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{}); });
    builder.phase("late",
            [](const TaskRecord&, TaskRuntime&) { return support::AsyncResult<void>{support::ExpectedVoid{}}; });
    builder.abort([](const TaskRecord&, TaskRuntime&) { return support::AsyncResult<void>{support::ExpectedVoid{}}; });
    builder.migrate([](const chord::Json& input, const chord::Json& checkpoint, std::int64_t) {
        return MigratedTaskState{.input = input, .checkpoint = checkpoint};
    });
    builder.hook("lateHook");
    CHECK_FALSE(builder.valid());

    auto second = builder.build();
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == support::ErrorCode::Validation);
    CHECK(second.error().message.find("consumes the builder") != std::string::npos);

    // A third call reports the same validation error rather than crashing.
    auto third = builder.build();
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error().code == support::ErrorCode::Validation);
}

TEST_CASE("TaskExecution: abort handler error faults the task with the handler message", "[durable][task][issue1076]") {
    auto def_builder = TaskDefinitionBuilder("audit1076.abort-error", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase(
            "run", [](const TaskRecord&, TaskRuntime&) { return support::AsyncResult<void>{support::ExpectedVoid{}}; });
    def_builder.abort([](const TaskRecord&, TaskRuntime&) {
        return support::AsyncResult<void>{
                std::unexpected(support::make_error(support::ErrorCode::Tool, "cleanup handler blew up"))};
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
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created =
                tx.create_task(*harness->task_definition("audit1076.abort-error"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    auto abort_res = consume(harness->abort_task(task_id));
    REQUIRE(abort_res.has_value());
    CHECK(abort_res.value() == "marked");

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();
    CHECK(receipt.state.status == "terminal");
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == "faulted");
    REQUIRE(receipt.state.outcome->error.has_value());
    CHECK(receipt.state.outcome->error->message == "cleanup handler blew up");

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: abort handler returning without a terminal outcome faults the task",
        "[durable][task][issue1076]") {
    std::atomic_bool abort_handler_ran{false};

    auto def_builder = TaskDefinitionBuilder("audit1076.abort-no-outcome", 1);
    def_builder.initial([](const chord::Json&) {
        chord::Json::Object obj;
        obj.emplace("phase", "run");
        return chord::Json(std::move(obj));
    });
    def_builder.phase(
            "run", [](const TaskRecord&, TaskRuntime&) { return support::AsyncResult<void>{support::ExpectedVoid{}}; });
    def_builder.abort([&](const TaskRecord&, TaskRuntime&) {
        abort_handler_ran.store(true);
        // Returns successfully without committing a terminal outcome.
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
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *harness->task_definition("audit1076.abort-no-outcome"), chord::Json(chord::Json::Object{}));
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    auto abort_res = consume(harness->abort_task(task_id));
    REQUIRE(abort_res.has_value());
    CHECK(abort_res.value() == "marked");

    REQUIRE(harness->resume().has_value());
    auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    // The abort handler ran on an execution thread; its observation is asserted
    // here, on the test thread, after the wait returned.
    CHECK(abort_handler_ran.load());
    CHECK(receipt.state.status == "terminal");
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == "faulted");
    REQUIRE(receipt.state.outcome->error.has_value());
    CHECK(receipt.state.outcome->error->message ==
            "Abort handler of task " + std::to_string(task_id.value) + " returned without a terminal outcome");

    REQUIRE(consume(harness->close()).has_value());
}

namespace {

const std::string kVersioningArtifact = "fixtures/pi-v1.3.0/audit-1055/task-versioning.json";
const std::string kLifecycleArtifact = "fixtures/pi-v1.3.0/audit-1055/task-lifecycle.json";

[[nodiscard]] TaskDefinition make_versioned_task(std::int64_t version,
        std::string result_prefix,
        MigrateHook migrate = {},
        std::vector<std::string>* log = nullptr,
        std::mutex* log_mutex = nullptr) {
    auto builder = TaskDefinitionBuilder("audit.versioned", version);
    builder.initial([](const chord::Json& input) {
        double n = 0.0;
        if (const auto* obj = input.as_object()) {
            if (auto it = obj->find("n"); it != obj->end() && it->second.as_number()) {
                n = *it->second.as_number();
            }
        }
        return chord::Json(chord::Json::Object{
                {"phase", chord::Json("run")},
                {"n", chord::Json(n)},
        });
    });
    builder.phase("run",
            [result_prefix = std::move(result_prefix), log, log_mutex](
                    const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
                if (log != nullptr) {
                    if (log_mutex != nullptr) {
                        std::lock_guard lock(*log_mutex);
                        log->push_back("run-v" + std::to_string(task.version));
                    } else {
                        log->push_back("run-v" + std::to_string(task.version));
                    }
                }
                double n = 0.0;
                if (task.state.checkpoint && task.state.checkpoint->as_object()) {
                    const auto& cp = *task.state.checkpoint->as_object();
                    if (auto it = cp.find("n"); it != cp.end() && it->second.as_number()) {
                        n = *it->second.as_number();
                    }
                }
                const std::string result = result_prefix + ":" + std::to_string(static_cast<int>(n));
                return runtime.commit(
                        [result](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            return NextTaskState::terminal(TaskOutcome{
                                    .status = "completed",
                                    .result = chord::Json(result),
                            });
                        });
            });
    builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });
    if (migrate) {
        builder.migrate(std::move(migrate));
    }
    return std::move(*builder.build());
}

} // namespace

TEST_CASE("TaskExecution: reservation migrates a stored definition before running the newer version",
        "[durable][task][issue1077]") {
    const auto scenario = FrozenScenario::load(
            kVersioningArtifact, "reservation migrates a stored definition before running the newer version");
    REQUIRE(scenario.has_value());

    std::mutex log_mutex;
    std::vector<std::string> log;

    Registry registry;
    Extension ext_v1{.name = "audit"};
    ext_v1.tasks.push_back(make_versioned_task(1, "v1", {}, &log, &log_mutex));
    REQUIRE(registry.install(std::move(ext_v1)).has_value());

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.registry = &registry}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* v1_def = harness->task_definition("audit.versioned");
        if (v1_def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.versioned"));
        }
        auto created = tx.create_task(*v1_def,
                chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    const auto before_inspect = consume(harness->inspect());
    REQUIRE(before_inspect.has_value());
    REQUIRE(before_inspect.value().tasks.size() == 1);
    const auto* expected_before = scenario->array_member("before");
    REQUIRE(expected_before != nullptr);
    REQUIRE(expected_before->size() == 1);
    CHECK(before_inspect.value().tasks[0].state.kind == expected_before->at(0).at("kind").get_string());
    REQUIRE(before_inspect.value().tasks[0].state.migrates.has_value());
    CHECK(*before_inspect.value().tasks[0].state.migrates == expected_before->at(0).at("migrates").get_boolean());

    Extension ext_v2{.name = "audit"};
    ext_v2.tasks.push_back(make_versioned_task(
            2,
            "v2",
            [](const chord::Json& input, const chord::Json& checkpoint, std::int64_t from_version)
                    -> support::Expected<MigratedTaskState> {
                const double in_n = *input.as_object()->at("n").as_number();
                const double cp_n = *checkpoint.as_object()->at("n").as_number();
                return MigratedTaskState{
                        .input = chord::Json(chord::Json::Object{
                                {"n", chord::Json(in_n + static_cast<double>(from_version))},
                        }),
                        .checkpoint = chord::Json(chord::Json::Object{
                                {"phase", chord::Json("run")},
                                {"n", chord::Json(cp_n + 10.0)},
                        }),
                };
            },
            &log,
            &log_mutex));
    REQUIRE(registry.install(std::move(ext_v2)).has_value());

    const auto after_install_inspect = consume(harness->inspect());
    REQUIRE(after_install_inspect.has_value());
    REQUIRE(after_install_inspect.value().tasks.size() == 1);
    const auto* expected_after_install = scenario->array_member("afterInstall");
    REQUIRE(expected_after_install != nullptr);
    REQUIRE(expected_after_install->size() == 1);
    CHECK(after_install_inspect.value().tasks[0].state.kind == expected_after_install->at(0).at("kind").get_string());
    REQUIRE(after_install_inspect.value().tasks[0].state.migrates.has_value());
    CHECK(*after_install_inspect.value().tasks[0].state.migrates ==
            expected_after_install->at(0).at("migrates").get_boolean());

    REQUIRE(harness->resume().has_value());
    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    const auto* expected_log = scenario->array_member("log");
    REQUIRE(expected_log != nullptr);
    REQUIRE(log.size() == expected_log->size());
    for (std::size_t i = 0; i < log.size(); ++i) {
        CHECK(log[i] == expected_log->at(i).get_string());
    }

    CHECK(receipt.version == static_cast<std::int64_t>(*scenario->number_member("version")));
    CHECK(json_equals(receipt.input, *scenario->member("input")));
    REQUIRE(receipt.state.outcome.has_value());
    const auto* expected_outcome = scenario->object_member("outcome");
    REQUIRE(expected_outcome != nullptr);
    CHECK(receipt.state.outcome->status == expected_outcome->at("status").get_string());
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(json_equals(*receipt.state.outcome->result, expected_outcome->at("result")));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: a task with no installed definition stays pending and runs when its definition returns",
        "[durable][task][issue1077]") {
    const auto scenario = FrozenScenario::load(kVersioningArtifact,
            "a task with no installed definition stays pending and runs when its definition returns");
    REQUIRE(scenario.has_value());

    Registry registry;
    Extension ext_initial{.name = "audit"};
    ext_initial.tasks.push_back(make_versioned_task(1, "v1"));
    REQUIRE(registry.install(std::move(ext_initial)).has_value());

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.registry = &registry}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* def = harness->task_definition("audit.versioned");
        if (def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.versioned"));
        }
        auto created = tx.create_task(*def,
                chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    registry.uninstall("audit");
    REQUIRE(harness->resume().has_value());

    const auto blocked_inspect = consume(harness->inspect());
    REQUIRE(blocked_inspect.has_value());
    REQUIRE(blocked_inspect.value().tasks.size() == 1);
    CHECK(blocked_inspect.value().tasks[0].state.kind == *scenario->string_member("inspection"));
    REQUIRE(blocked_inspect.value().tasks[0].state.reason.has_value());
    CHECK(*blocked_inspect.value().tasks[0].state.reason == *scenario->string_member("reason"));

    const auto record_res = consume(harness->get_task(task_id));
    REQUIRE(record_res.has_value());
    REQUIRE(record_res.value().has_value());
    CHECK(record_res.value()->state.status == *scenario->string_member("recordStatus"));

    Extension ext_restored{.name = "audit"};
    ext_restored.tasks.push_back(make_versioned_task(1, "v1"));
    REQUIRE(registry.install(std::move(ext_restored)).has_value());

    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();
    REQUIRE(receipt.state.outcome.has_value());
    const auto* expected_outcome = scenario->object_member("outcome");
    REQUIRE(expected_outcome != nullptr);
    CHECK(receipt.state.outcome->status == expected_outcome->at("status").get_string());
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(json_equals(*receipt.state.outcome->result, expected_outcome->at("result")));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: a task whose stored version is newer than the installed definition stays pending and blocked "
          "then recovers",
        "[durable][task][issue1077]") {
    const auto scenario = FrozenScenario::load(kVersioningArtifact,
            "a task whose stored version is newer than the installed definition stays pending and blocked");
    REQUIRE(scenario.has_value());

    Registry registry;
    Extension ext_v2{.name = "audit"};
    ext_v2.tasks.push_back(make_versioned_task(2,
            "v2",
            [](const chord::Json& input, const chord::Json& checkpoint, std::int64_t)
                    -> support::Expected<MigratedTaskState> {
                return MigratedTaskState{.input = input, .checkpoint = checkpoint};
            }));
    REQUIRE(registry.install(std::move(ext_v2)).has_value());

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.registry = &registry}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* def = harness->task_definition("audit.versioned");
        if (def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.versioned"));
        }
        auto created = tx.create_task(*def,
                chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    Extension ext_v1{.name = "audit"};
    ext_v1.tasks.push_back(make_versioned_task(1, "v1"));
    REQUIRE(registry.install(std::move(ext_v1)).has_value());

    REQUIRE(harness->resume().has_value());

    const auto blocked_inspect = consume(harness->inspect());
    REQUIRE(blocked_inspect.has_value());
    REQUIRE(blocked_inspect.value().tasks.size() == 1);
    CHECK(blocked_inspect.value().tasks[0].state.kind == *scenario->string_member("inspection"));
    REQUIRE(blocked_inspect.value().tasks[0].state.reason.has_value());
    CHECK(*blocked_inspect.value().tasks[0].state.reason == *scenario->string_member("reason"));

    const auto record_res = consume(harness->get_task(task_id));
    REQUIRE(record_res.has_value());
    REQUIRE(record_res.value().has_value());
    CHECK(record_res.value()->state.status == *scenario->string_member("recordStatus"));
    REQUIRE(record_res.value()->state.checkpoint.has_value());
    CHECK(json_equals(*record_res.value()->state.checkpoint, *scenario->member("checkpoint")));

    // Repairing the registry with version 2 allows the same pending task to complete.
    Extension ext_repaired{.name = "audit"};
    ext_repaired.tasks.push_back(make_versioned_task(2, "v2"));
    REQUIRE(registry.install(std::move(ext_repaired)).has_value());

    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    REQUIRE(receipt_res.value().state.outcome.has_value());
    CHECK(receipt_res.value().state.outcome->status == "completed");
    REQUIRE(receipt_res.value().state.outcome->result.has_value());
    CHECK(receipt_res.value().state.outcome->result == std::optional<chord::Json>{chord::Json("v2:1")});

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: a failed migration is reported once and leaves the task pending until a usable definition is "
          "installed",
        "[durable][task][issue1077]") {
    const auto scenario = FrozenScenario::load(kVersioningArtifact,
            "a failed migration is reported and leaves the task pending until a usable definition is installed");
    REQUIRE(scenario.has_value());

    std::mutex report_mutex;
    std::condition_variable report_cv;
    std::vector<std::string> reports;
    std::atomic_int broken_migration_attempts{0};

    Registry registry;
    Extension ext_v1{.name = "audit"};
    ext_v1.tasks.push_back(make_versioned_task(1, "v1"));
    REQUIRE(registry.install(std::move(ext_v1)).has_value());

    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.on_report = [&](const support::Error& error) {
        std::lock_guard lock(report_mutex);
        reports.push_back(error.message);
        report_cv.notify_all();
    };
    auto harness_res = consume(Harness::open(storage, std::move(options)));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* def = harness->task_definition("audit.versioned");
        if (def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.versioned"));
        }
        auto created = tx.create_task(*def,
                chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    Extension ext_broken{.name = "audit"};
    ext_broken.tasks.push_back(make_versioned_task(2,
            "broken",
            [&broken_migration_attempts](
                    const chord::Json&, const chord::Json&, std::int64_t) -> support::Expected<MigratedTaskState> {
                ++broken_migration_attempts;
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "cannot migrate"));
            }));
    REQUIRE(registry.install(std::move(ext_broken)).has_value());

    REQUIRE(harness->resume().has_value());
    {
        std::unique_lock lock(report_mutex);
        REQUIRE(report_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return !reports.empty(); }));
    }

    // An unrelated extension install wakes the scheduler without retrying the same failed definition.
    Extension ext_unrelated{.name = "unrelated"};
    ext_unrelated.sections.push_back(PromptSection{.key = "note", .text = "unrelated"});
    REQUIRE(registry.install(std::move(ext_unrelated)).has_value());

    const auto blocked_inspect = consume(harness->inspect());
    REQUIRE(blocked_inspect.has_value());
    REQUIRE(blocked_inspect.value().tasks.size() == 1);
    CHECK(blocked_inspect.value().tasks[0].state.kind == *scenario->string_member("blocked"));
    REQUIRE(blocked_inspect.value().tasks[0].state.reason.has_value());
    CHECK(*blocked_inspect.value().tasks[0].state.reason == *scenario->string_member("reason"));
    CHECK(broken_migration_attempts.load() == 1);

    const auto pending_record = consume(harness->get_task(task_id));
    REQUIRE(pending_record.has_value());
    REQUIRE(pending_record.value().has_value());
    CHECK(pending_record.value()->state.status == "pending");
    CHECK(pending_record.value()->version == 1);

    Extension ext_good{.name = "audit"};
    ext_good.tasks.push_back(make_versioned_task(2,
            "v2",
            [](const chord::Json& input, const chord::Json& checkpoint, std::int64_t)
                    -> support::Expected<MigratedTaskState> {
                return MigratedTaskState{.input = input, .checkpoint = checkpoint};
            }));
    REQUIRE(registry.install(std::move(ext_good)).has_value());

    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();
    CHECK(receipt.version == 2);
    REQUIRE(receipt.state.outcome.has_value());
    const auto* expected_outcome = scenario->object_member("recoveredOutcome");
    REQUIRE(expected_outcome != nullptr);
    CHECK(receipt.state.outcome->status == expected_outcome->at("status").get_string());
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(json_equals(*receipt.state.outcome->result, expected_outcome->at("result")));

    {
        std::lock_guard lock(report_mutex);
        const auto* expected_reports = scenario->array_member("reports");
        REQUIRE(expected_reports != nullptr);
        REQUIRE(reports.size() == expected_reports->size());
        for (std::size_t i = 0; i < reports.size(); ++i) {
            CHECK(reports[i] == expected_reports->at(i).get_string());
        }
    }

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: aborting a task blocked by too-old or failed-migration definition commits an orphaned "
          "outcome with the blocked reason",
        "[durable][task][issue1077]") {
    Registry registry;
    Extension ext_v2{.name = "audit"};
    ext_v2.tasks.push_back(make_versioned_task(2, "v2"));
    REQUIRE(registry.install(std::move(ext_v2)).has_value());

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.registry = &registry}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId too_old_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* def = harness->task_definition("audit.versioned");
        if (def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.versioned"));
        }
        auto created = tx.create_task(*def,
                chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        too_old_id = *created;
        return {};
    })).has_value());

    // Downgrade definition to v1: too_old_id (v2) is now blocked with task_too_old.
    // Also create a v1 task that will fail migration when v2 without migrate is installed.
    Extension ext_v1{.name = "audit"};
    ext_v1.tasks.push_back(make_versioned_task(1, "v1"));
    REQUIRE(registry.install(std::move(ext_v1)).has_value());

    TaskId failed_mig_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* def = harness->task_definition("audit.versioned");
        if (def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.versioned"));
        }
        auto created = tx.create_task(*def,
                chord::Json(chord::Json::Object{{"n", chord::Json(1.0)}}),
                TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        failed_mig_id = *created;
        return {};
    })).has_value());

    const auto abort_too_old = consume(harness->abort_task(too_old_id));
    REQUIRE(abort_too_old.has_value());
    CHECK(abort_too_old.value() == "marked");
    const auto too_old_receipt = consume(harness->wait_for_task(too_old_id));
    REQUIRE(too_old_receipt.has_value());
    CHECK(too_old_receipt.value().state.status == "terminal");
    REQUIRE(too_old_receipt.value().state.outcome.has_value());
    CHECK(too_old_receipt.value().state.outcome->status == "orphaned");
    CHECK(too_old_receipt.value().state.outcome->reason == std::optional<std::string>{"task_too_old"});

    // Install v2 without a migration callback: failed_mig_id (v1) is now blocked with migration_failed.
    Extension ext_v2_no_mig{.name = "audit"};
    ext_v2_no_mig.tasks.push_back(make_versioned_task(2, "v2"));
    REQUIRE(registry.install(std::move(ext_v2_no_mig)).has_value());

    const auto abort_failed_mig = consume(harness->abort_task(failed_mig_id));
    REQUIRE(abort_failed_mig.has_value());
    CHECK(abort_failed_mig.value() == "marked");
    const auto failed_mig_receipt = consume(harness->wait_for_task(failed_mig_id));
    REQUIRE(failed_mig_receipt.has_value());
    CHECK(failed_mig_receipt.value().state.status == "terminal");
    REQUIRE(failed_mig_receipt.value().state.outcome.has_value());
    CHECK(failed_mig_receipt.value().state.outcome->status == "orphaned");
    CHECK(failed_mig_receipt.value().state.outcome->reason == std::optional<std::string>{"migration_failed"});

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: a running task hands over to a newer definition at a phase boundary without mid-flight "
          "takeover",
        "[durable][task][issue1077]") {
    const auto scenario = FrozenScenario::load(
            kVersioningArtifact, "a running task hands over to a newer definition at a phase boundary");
    REQUIRE(scenario.has_value());

    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool v1_p1_entered = false;
    bool release_v1_p1 = false;
    std::vector<std::string> log;
    std::string memo_seen_in_v2;

    auto record_log = [&](std::string entry) {
        std::lock_guard lock(sync_mutex);
        log.push_back(std::move(entry));
    };

    auto v1_builder = TaskDefinitionBuilder("audit.handover", 1);
    v1_builder.initial(
            [](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", chord::Json("p1")}}); });
    v1_builder.phase("p1", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto memo_res = consume(runtime.memo("picked", chord::Json("v1")));
        if (!memo_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(memo_res.error())};
        }
        auto commit_res = consume(
                runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::running(chord::Json(chord::Json::Object{{"phase", chord::Json("p2")}}));
                }));
        if (!commit_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(commit_res.error())};
        }
        {
            std::unique_lock lock(sync_mutex);
            log.push_back("v1-p1");
            v1_p1_entered = true;
            sync_cv.notify_all();
            if (!sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return release_v1_p1; })) {
                return support::AsyncResult<void>{std::unexpected(
                        support::make_error(support::ErrorCode::Timeout, "Timed out waiting for handover gate"))};
            }
            log.push_back("v1-p1-release");
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    v1_builder.phase("p2", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        record_log("v1-p2-unexpected");
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("v1")});
        });
    });
    v1_builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });

    auto v2_builder = TaskDefinitionBuilder("audit.handover", 2);
    v2_builder.initial(
            [](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", chord::Json("p1")}}); });
    v2_builder.phase("p1", [&](const TaskRecord&, TaskRuntime&) -> support::AsyncResult<void> {
        record_log("v2-p1-unexpected");
        return support::AsyncResult<void>{std::unexpected(
                support::make_error(support::ErrorCode::Validation, "version 2 phase p1 must not run"))};
    });
    v2_builder.phase("p2", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto memo_res = consume(runtime.memo("picked", chord::Json("v2")));
        if (!memo_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(memo_res.error())};
        }
        {
            std::lock_guard lock(sync_mutex);
            if (const auto* str = memo_res.value().as_string()) {
                memo_seen_in_v2 = *str;
            }
            log.push_back("v2-p2");
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("v2")});
        });
    });
    v2_builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });
    v2_builder.migrate([&](const chord::Json& input,
                               const chord::Json& checkpoint,
                               std::int64_t from_version) -> support::Expected<MigratedTaskState> {
        record_log("v2-migrate-from-" + std::to_string(from_version));
        return MigratedTaskState{.input = input, .checkpoint = checkpoint};
    });

    Registry registry;
    Extension ext_v1{.name = "audit"};
    ext_v1.tasks.push_back(std::move(*v1_builder.build()));
    REQUIRE(registry.install(std::move(ext_v1)).has_value());

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.registry = &registry}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto* def = harness->task_definition("audit.handover");
        if (def == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Missing audit.handover"));
        }
        auto created = tx.create_task(*def, chord::Json{nullptr}, TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    REQUIRE(harness->resume().has_value());
    {
        std::unique_lock lock(sync_mutex);
        REQUIRE(sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return v1_p1_entered; }));
    }

    // Install v2 while v1 is still running p1: v2 must NOT take over mid-phase.
    Extension ext_v2{.name = "audit"};
    ext_v2.tasks.push_back(std::move(*v2_builder.build()));
    REQUIRE(registry.install(std::move(ext_v2)).has_value());
    {
        std::lock_guard lock(sync_mutex);
        CHECK(log == std::vector<std::string>{"v1-p1"});
    }

    {
        std::lock_guard lock(sync_mutex);
        release_v1_p1 = true;
        sync_cv.notify_all();
    }

    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    const auto* expected_log = scenario->array_member("log");
    REQUIRE(expected_log != nullptr);
    {
        std::lock_guard lock(sync_mutex);
        REQUIRE(log.size() == expected_log->size());
        for (std::size_t i = 0; i < log.size(); ++i) {
            CHECK(log[i] == expected_log->at(i).get_string());
        }
        CHECK(memo_seen_in_v2 == "v1");
    }

    CHECK(receipt.version == static_cast<std::int64_t>(*scenario->number_member("version")));
    REQUIRE(receipt.state.outcome.has_value());
    const auto* expected_outcome = scenario->object_member("outcome");
    REQUIRE(expected_outcome != nullptr);
    CHECK(receipt.state.outcome->status == expected_outcome->at("status").get_string());
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(json_equals(*receipt.state.outcome->result, expected_outcome->at("result")));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: a running task keeps its old definition when a replacement is missing or lacks a migration",
        "[durable][task][issue1077]") {
    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool p1_entered = false;
    bool release_p1 = false;
    bool p2_entered = false;
    bool release_p2 = false;
    std::vector<std::string> log;
    std::vector<std::string> reports;

    auto v1_builder = TaskDefinitionBuilder("audit.keep-old", 1);
    v1_builder.initial(
            [](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", chord::Json("p1")}}); });
    v1_builder.phase("p1", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto commit_res = consume(
                runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::running(chord::Json(chord::Json::Object{{"phase", chord::Json("p2")}}));
                }));
        if (!commit_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(commit_res.error())};
        }
        std::unique_lock lock(sync_mutex);
        log.push_back("v1-p1");
        p1_entered = true;
        sync_cv.notify_all();
        if (!sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return release_p1; })) {
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Timeout, "Timed out in p1"))};
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    v1_builder.phase("p2", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto commit_res = consume(
                runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    return NextTaskState::running(chord::Json(chord::Json::Object{{"phase", chord::Json("p3")}}));
                }));
        if (!commit_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(commit_res.error())};
        }
        std::unique_lock lock(sync_mutex);
        log.push_back("v1-p2");
        p2_entered = true;
        sync_cv.notify_all();
        if (!sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return release_p2; })) {
            return support::AsyncResult<void>{
                    std::unexpected(support::make_error(support::ErrorCode::Timeout, "Timed out in p2"))};
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    v1_builder.phase("p3", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        {
            std::lock_guard lock(sync_mutex);
            log.push_back("v1-p3");
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("v1")});
        });
    });

    Registry registry;
    Extension ext_v1{.name = "audit"};
    ext_v1.tasks.push_back(std::move(*v1_builder.build()));
    REQUIRE(registry.install(std::move(ext_v1)).has_value());

    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.on_report = [&](const support::Error& err) {
        std::lock_guard lock(sync_mutex);
        reports.push_back(err.message);
    };
    auto harness_res = consume(Harness::open(storage, std::move(options)));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*harness->task_definition("audit.keep-old"), chord::Json{nullptr});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    REQUIRE(harness->resume().has_value());
    {
        std::unique_lock lock(sync_mutex);
        REQUIRE(sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return p1_entered; }));
    }

    // Uninstall definition while p1 is running; at p1 -> p2 boundary, v1 keeps running and reports once.
    registry.uninstall("audit");
    {
        std::lock_guard lock(sync_mutex);
        release_p1 = true;
        sync_cv.notify_all();
    }
    {
        std::unique_lock lock(sync_mutex);
        REQUIRE(sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return p2_entered; }));
    }

    // Install v2 WITHOUT a migrate handler while p2 is running; at p2 -> p3 boundary, v1 keeps running and reports
    // once.
    auto v2_incompatible = TaskDefinitionBuilder("audit.keep-old", 2)
                                   .initial([](const chord::Json&) {
                                       return chord::Json(chord::Json::Object{{"phase", chord::Json("p1")}});
                                   })
                                   .phase("p3",
                                           [&](const TaskRecord&, TaskRuntime&) -> support::AsyncResult<void> {
                                               std::lock_guard lock(sync_mutex);
                                               log.push_back("v2-p3-unexpected");
                                               return support::AsyncResult<void>{support::ExpectedVoid{}};
                                           })
                                   .build();
    REQUIRE(v2_incompatible.has_value());
    Extension ext_v2{.name = "audit"};
    ext_v2.tasks.push_back(std::move(*v2_incompatible));
    REQUIRE(registry.install(std::move(ext_v2)).has_value());

    {
        std::lock_guard lock(sync_mutex);
        release_p2 = true;
        sync_cv.notify_all();
    }

    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    CHECK(receipt_res.value().version == 1);
    REQUIRE(receipt_res.value().state.outcome.has_value());
    CHECK(receipt_res.value().state.outcome->status == "completed");
    CHECK(receipt_res.value().state.outcome->result == std::optional<chord::Json>{chord::Json("v1")});

    {
        std::lock_guard lock(sync_mutex);
        CHECK(log == std::vector<std::string>{"v1-p1", "v1-p2", "v1-p3"});
        REQUIRE(reports.size() == 2);
        const std::string expected_msg =
                "Task " + std::to_string(task_id.value) + " keeps running under its old audit.keep-old definition";
        CHECK(reports[0] == expected_msg);
        CHECK(reports[1] == expected_msg);
    }

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "TaskExecution: task hooks run in extension order and an ordinary failure is reported while the task continues",
        "[durable][task][issue1077]") {
    const auto scenario = FrozenScenario::load(kLifecycleArtifact,
            "task hooks run in extension order and an ordinary throw is reported while the task continues");
    REQUIRE(scenario.has_value());

    std::mutex log_mutex;
    std::vector<std::string> log;
    std::vector<std::string> reports;

    auto hooked_builder = TaskDefinitionBuilder("audit.hooked", 1);
    hooked_builder.initial(
            [](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", chord::Json("run")}}); });
    hooked_builder.hook("onStep");
    hooked_builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto hook_res = consume(runtime.each_hook("onStep"));
        if (!hook_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(hook_res.error())};
        }
        {
            std::lock_guard lock(log_mutex);
            log.push_back("task");
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{
                    .status = "completed",
                    .result = chord::Json("done"),
            });
        });
    });
    hooked_builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });

    auto hooked_def = hooked_builder.build();
    REQUIRE(hooked_def.has_value());
    CHECK(hooked_def->has_hook("onStep"));
    CHECK_FALSE(hooked_def->has_hook("unknown"));

    Registry registry;
    Extension ext_tasks{.name = "audit"};
    ext_tasks.tasks.push_back(std::move(*hooked_def));
    REQUIRE(registry.install(std::move(ext_tasks)).has_value());

    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.on_report = [&](const support::Error& err) {
        std::lock_guard lock(log_mutex);
        reports.push_back(err.message);
    };
    auto harness_res = consume(Harness::open(storage, std::move(options)));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    const auto* registered_hooked = harness->task_definition("audit.hooked");
    REQUIRE(registered_hooked != nullptr);

    Extension ext_throw{.name = "hook-throw"};
    ext_throw.hooks.push_back(task_hook(*registered_hooked, "onStep", [&]() -> support::ExpectedVoid {
        {
            std::lock_guard lock(log_mutex);
            log.push_back("hook-1");
        }
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "hook boom"));
    }));
    REQUIRE(registry.install(std::move(ext_throw)).has_value());

    Extension ext_observer{.name = "hook-observer"};
    ext_observer.hooks.push_back(task_hook(*registered_hooked, "onStep", [&]() -> support::ExpectedVoid {
        std::lock_guard lock(log_mutex);
        log.push_back("hook-2");
        return {};
    }));
    REQUIRE(registry.install(std::move(ext_observer)).has_value());

    auto root_res = consume(harness->root());
    REQUIRE(root_res.has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(
                *registered_hooked, chord::Json{nullptr}, TaskOptions{.conversation_id = root_res.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    REQUIRE(harness->resume().has_value());
    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();

    {
        std::lock_guard lock(log_mutex);
        const auto* expected_log = scenario->array_member("log");
        REQUIRE(expected_log != nullptr);
        REQUIRE(log.size() == expected_log->size());
        for (std::size_t i = 0; i < log.size(); ++i) {
            CHECK(log[i] == expected_log->at(i).get_string());
        }

        const auto* expected_reports = scenario->array_member("reports");
        REQUIRE(expected_reports != nullptr);
        REQUIRE(reports.size() == expected_reports->size());
        for (std::size_t i = 0; i < reports.size(); ++i) {
            CHECK(reports[i] == expected_reports->at(i).get_string());
        }
    }

    REQUIRE(receipt.state.outcome.has_value());
    const auto* expected_outcome = scenario->object_member("outcome");
    REQUIRE(expected_outcome != nullptr);
    CHECK(receipt.state.outcome->status == expected_outcome->at("status").get_string());
    REQUIRE(receipt.state.outcome->result.has_value());
    CHECK(json_equals(*receipt.state.outcome->result, expected_outcome->at("result")));

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE(
        "TaskExecution: a hook failure on an aborted invocation stops subsequent hooks and does not continue the phase",
        "[durable][task][issue1077]") {
    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool hook1_started = false;
    std::vector<std::string> log;
    std::vector<std::string> reports;

    auto hooked_builder = TaskDefinitionBuilder("audit.hooked-abort", 1);
    hooked_builder.initial(
            [](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", chord::Json("run")}}); });
    hooked_builder.hook("onStep");
    hooked_builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto hook_res = consume(runtime.each_hook("onStep"));
        if (!hook_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(hook_res.error())};
        }
        {
            std::lock_guard lock(sync_mutex);
            log.push_back("task-unexpected");
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("unexpected")});
        });
    });
    hooked_builder.abort([&](const TaskRecord&, TaskRuntime& runtime) {
        {
            std::lock_guard lock(sync_mutex);
            log.push_back("abort");
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted", .reason = "cancelled"});
        });
    });

    Registry registry;
    Extension ext_tasks{.name = "audit"};
    ext_tasks.tasks.push_back(std::move(*hooked_builder.build()));
    REQUIRE(registry.install(std::move(ext_tasks)).has_value());

    MemoryStorage storage;
    HarnessOptions options{.registry = &registry};
    options.on_report = [&](const support::Error& err) {
        std::lock_guard lock(sync_mutex);
        reports.push_back(err.message);
    };
    auto harness_res = consume(Harness::open(storage, std::move(options)));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    const auto* registered_hooked = harness->task_definition("audit.hooked-abort");
    REQUIRE(registered_hooked != nullptr);

    Extension ext_first{.name = "hook-1"};
    ext_first.hooks.push_back(task_hook(*registered_hooked,
            "onStep",
            [&](const chord::Json&, TaskRuntime& runtime) -> support::AsyncResult<chord::Json> {
                const std::stop_token stop = runtime.stop_token();
                std::unique_lock lock(sync_mutex);
                hook1_started = true;
                sync_cv.notify_all();
                std::stop_callback wake_on_stop{stop, [&sync_cv] { sync_cv.notify_all(); }};
                if (!sync_cv.wait_for(lock, std::chrono::seconds{5}, [&stop] { return stop.stop_requested(); })) {
                    return support::AsyncResult<chord::Json>{std::unexpected(
                            support::make_error(support::ErrorCode::Timeout, "Timed out waiting for stop"))};
                }
                log.push_back("hook-1-aborted");
                return support::AsyncResult<chord::Json>{
                        std::unexpected(support::make_error(support::ErrorCode::Cancelled, "cancelled in hook"))};
            }));
    REQUIRE(registry.install(std::move(ext_first)).has_value());

    Extension ext_second{.name = "hook-2"};
    ext_second.hooks.push_back(task_hook(*registered_hooked, "onStep", [&]() -> support::ExpectedVoid {
        std::lock_guard lock(sync_mutex);
        log.push_back("hook-2-unexpected");
        return {};
    }));
    REQUIRE(registry.install(std::move(ext_second)).has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*registered_hooked, chord::Json{nullptr});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    REQUIRE(harness->resume().has_value());
    {
        std::unique_lock lock(sync_mutex);
        REQUIRE(sync_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return hook1_started; }));
    }

    const auto abort_res = consume(harness->abort_task(task_id));
    REQUIRE(abort_res.has_value());
    CHECK(abort_res.value() == "marked");

    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    const auto& receipt = receipt_res.value();
    CHECK(receipt.state.status == "terminal");
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == "aborted");
    CHECK(receipt.state.outcome->reason == std::optional<std::string>{"cancelled"});

    {
        std::lock_guard lock(sync_mutex);
        CHECK(log == std::vector<std::string>{"hook-1-aborted", "abort"});
        CHECK(reports.empty());
    }

    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("TaskExecution: replacing an extension in place preserves hook order while uninstall and reinstall moves it "
          "to the end",
        "[durable][task][issue1077]") {
    std::mutex log_mutex;
    std::vector<std::string> log;

    auto hooked_builder = TaskDefinitionBuilder("audit.hook-order", 1);
    hooked_builder.initial(
            [](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", chord::Json("run")}}); });
    hooked_builder.hook("onStep");
    hooked_builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        auto hook_res = consume(runtime.each_hook("onStep"));
        if (!hook_res.has_value()) {
            return support::AsyncResult<void>{std::unexpected(hook_res.error())};
        }
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json("ok")});
        });
    });

    Registry registry;
    Extension ext_tasks{.name = "tasks"};
    ext_tasks.tasks.push_back(std::move(*hooked_builder.build()));
    REQUIRE(registry.install(std::move(ext_tasks)).has_value());

    // A second extension trying to register the same task kind or reserved pi.generation is rejected.
    Extension conflict_ext{.name = "other-tasks"};
    conflict_ext.tasks.push_back(std::move(*TaskDefinitionBuilder("audit.hook-order", 1).build()));
    CHECK_FALSE(registry.install(std::move(conflict_ext)).has_value());

    Extension reserved_ext{.name = "reserved-tasks"};
    reserved_ext.tasks.push_back(std::move(*TaskDefinitionBuilder("pi.generation", 1).build()));
    CHECK_FALSE(registry.install(std::move(reserved_ext)).has_value());

    MemoryStorage storage;
    auto harness_res = consume(Harness::open(storage, HarnessOptions{.registry = &registry}));
    REQUIRE(harness_res.has_value());
    auto harness = std::move(harness_res.value());

    const auto* def = harness->task_definition("audit.hook-order");
    REQUIRE(def != nullptr);

    auto make_hook_ext = [&](std::string ext_name, std::string entry) {
        Extension ext{.name = std::move(ext_name)};
        ext.hooks.push_back(task_hook(*def, "onStep", [&, entry = std::move(entry)]() -> support::ExpectedVoid {
            std::lock_guard lock(log_mutex);
            log.push_back(entry);
            return {};
        }));
        return ext;
    };

    REQUIRE(registry.install(make_hook_ext("ext-a", "a-v1")).has_value());
    REQUIRE(registry.install(make_hook_ext("ext-b", "b-v1")).has_value());
    REQUIRE(registry.install(make_hook_ext("ext-c", "c-v1")).has_value());

    // Replace ext-a in place: stays at index 0.
    REQUIRE(registry.install(make_hook_ext("ext-a", "a-v2")).has_value());
    // Uninstall ext-b and reinstall it: moves to the end after ext-c.
    registry.uninstall("ext-b");
    REQUIRE(registry.install(make_hook_ext("ext-b", "b-v2")).has_value());

    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*def, chord::Json{nullptr});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        return {};
    })).has_value());

    REQUIRE(harness->resume().has_value());
    const auto receipt_res = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt_res.has_value());
    CHECK(receipt_res.value().state.status == "terminal");

    {
        std::lock_guard lock(log_mutex);
        CHECK(log == std::vector<std::string>{"a-v2", "c-v1", "b-v2"});
    }

    REQUIRE(consume(harness->close()).has_value());
}

} // namespace cch::tests::durable
