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
    CHECK(*running_memos_seen["requested"].as_number() == 7.0);

    // Terminal memo check: memos must be empty on terminal settlement
    CHECK(receipt.memos.empty());
    CHECK(receipt.state.status == *scenario->string_member("terminalStatus"));
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == *scenario->string_member("outcomeStatus"));

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
    CHECK(abort_res.has_value());
    if (abort_res.has_value()) {
        CHECK(abort_res.value() == *scenario->string_member("abortResult"));
    }
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

    CHECK(receipt.state.status == *scenario->string_member("status"));
    REQUIRE(receipt.state.outcome.has_value());
    CHECK(receipt.state.outcome->status == *scenario->string_member("outcomeStatus"));
    REQUIRE(receipt.state.outcome->error.has_value());
    CHECK(receipt.state.outcome->error->message == *scenario->string_member("errorMessage"));

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
            const auto* cp = task.state.checkpoint->as_object();
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
        CHECK(receipt.state.status == *scenario->string_member("status"));
        REQUIRE(receipt.state.outcome.has_value());
        CHECK(receipt.state.outcome->status == "completed");
        CHECK(*receipt.state.outcome->result->as_number() == *scenario->number_member("result"));

        // Discriminating check: p1 must NOT have run after reopen!
        REQUIRE(reopened_log.size() == 1);
        CHECK(reopened_log[0] == "p2:1");

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
    CHECK(*receipt.state.outcome->result->as_object()->at("receiptId").as_number() == 999.0);

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

} // namespace cch::tests::durable
