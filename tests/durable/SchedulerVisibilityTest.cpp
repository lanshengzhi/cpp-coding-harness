#include <cch/durable/Harness.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>

#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {
/// Rejects the reservation write (`pending` -> `running`) of one task kind,
/// leaving every other write (including the task's creation) untouched.
class RejectReservationStorage final : public Storage {
public:
    explicit RejectReservationStorage(Storage& inner) : inner_(inner) {}

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        for (const auto& write : writes) {
            if (write.type == "task" && write.task.has_value() && write.task->kind == "audit1165.rejected" &&
                    write.task->state.status == "running") {
                return support::AsyncResult<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Process, "injected 1165 reservation failure"))};
            }
        }
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

    [[nodiscard]] support::Expected<std::vector<DocumentRecord>> scan_conversation_documents(
            ConversationId conversation_id, DocumentPoint at, std::size_t limit) override {
        return inner_.scan_conversation_documents(conversation_id, at, limit);
    }

    [[nodiscard]] support::Expected<std::optional<DocumentRecord>> find_document(
            std::string_view kind, const DocumentAddress& address, DocumentPoint at) override {
        return inner_.find_document(kind, address, at);
    }

    [[nodiscard]] support::Expected<std::optional<StoredDocument>> document(DocumentId id, DocumentPoint at) override {
        return inner_.document(id, at);
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

private:
    Storage& inner_; // outlives this wrapper
};
} // namespace

// The visibility defect (issue #1165): the scheduler used to register the
// invocation before its reservation commit was admitted, so an inspect whose
// scan predated the commit derived `running` from a record that was still
// committed `pending`. The reservation now publishes its invocation in the
// commit completion on the Session line, after Storage settlement; the gate
// below additionally refuses to derive running from an identity that is newer
// than the scanned record.
TEST_CASE("scheduler reservation is invisible to a read admitted before its commit", "[durable][issue1165]") {
    MemoryStorage storage;
    auto builder = TaskDefinitionBuilder("audit1165.reservation", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.reservation"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::atomic_bool inconsistent{false};
    // The holder and probe borrow the flags, storage, and harness by
    // reference; both threads are joined before any of them leave scope, and
    // their internal loops are bounded by the same `release` flag.
    std::jthread holder([&harness, &entered, &release] {
        static_cast<void>(consume(
                harness->session().read_committed([&entered, &release](Storage&) -> support::Expected<chord::Json> {
                    entered.store(true);
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    while (!release.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::yield();
                    return chord::Json(nullptr);
                })));
    });
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load() && std::chrono::steady_clock::now() < start_deadline)
        std::this_thread::yield();
    const bool did_enter = entered.load();
    std::jthread probe([&did_enter, &harness, &release, &inconsistent, &storage, &id] {
        if (!did_enter || !harness->resume()) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!release.load() && std::chrono::steady_clock::now() < deadline) {
            auto inspection = consume(harness->inspect());
            auto record = storage.task(id);
            if (inspection.has_value() && record && record->has_value()) {
                for (const auto& row : inspection.value().tasks) {
                    if (row.record.id == id && row.record.state.status == "pending" && row.state.kind == "running" &&
                            (*record)->state.status == "pending") {
                        inconsistent.store(true);
                    }
                }
            }
            if (inconsistent.load()) break;
            std::this_thread::yield();
        }
    });
    const auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!inconsistent.load() && std::chrono::steady_clock::now() < probe_deadline)
        std::this_thread::yield();
    release.store(true);
    holder.join();
    probe.join();
    CHECK(did_enter);
    CHECK_FALSE(inconsistent.load());
    CHECK(consume(harness->close()).has_value());
}

// The frozen waiting contract (three independent frozen runs, issue #1165):
// a phase that commits `waiting` and remains active is observed as
// `record.status=waiting` with `state.kind=running`. Running must not be
// derived from the committed status alone, and the parked record must return
// to a non-running derived state once the invocation ends.
TEST_CASE("a phase that commits waiting while remaining active is reported running", "[durable][issue1165][waiting]") {
    MemoryStorage storage;
    std::atomic_bool waiting_committed{false};
    std::atomic_bool release_phase{false};
    std::atomic_bool observed_running_while_waiting{false};
    auto builder = TaskDefinitionBuilder("audit1165.waiting", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase runs on a harness execution thread and borrows the flags by
    // reference; the harness is closed (joining that thread) before the flags
    // leave scope, so every callback run sees live state.
    builder.phase("run",
            [&waiting_committed, &release_phase](
                    const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                if (waiting_committed.load()) {
                    // The scheduler resumed the parked wait: finish so close can drain.
                    return runtime.commit(
                            [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                                return NextTaskState::terminal(TaskOutcome{.status = "completed"});
                            });
                }
                auto committed = consume(runtime.commit(
                        [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            return NextTaskState::waiting(
                                    chord::Json(chord::Json::Object{{"phase", "parked"}}), {}, JoinPolicy::AllSettled);
                        }));
                if (!committed.has_value()) {
                    return support::AsyncResult<void>{std::unexpected(committed.error())};
                }
                waiting_committed.store(true);
                // The invocation stays active while the record is committed waiting.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!release_phase.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.waiting"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());

    // Observe the frozen contract while the phase stays active (bounded).
    const auto observe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!observed_running_while_waiting.load() && std::chrono::steady_clock::now() < observe_deadline) {
        if (!waiting_committed.load()) {
            std::this_thread::yield();
            continue;
        }
        auto inspection = consume(harness->inspect());
        if (inspection.has_value()) {
            for (const auto& row : inspection.value().tasks) {
                if (row.record.id == id && row.record.state.status == "waiting" && row.state.kind == "running") {
                    observed_running_while_waiting.store(true);
                }
            }
        }
        std::this_thread::yield();
    }
    release_phase.store(true);

    // The parked wait drains once the invocation ends; the scheduler then
    // resumes and the phase's second run settles the task terminal.
    bool terminal = false;
    const auto terminal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!terminal && std::chrono::steady_clock::now() < terminal_deadline) {
        auto record = storage.task(id);
        if (record && record->has_value() && (*record)->state.status == "terminal") {
            terminal = true;
        }
        std::this_thread::yield();
    }
    const auto close_outcome = consume(harness->close());

    CHECK(observed_running_while_waiting.load());
    CHECK(terminal);
    CHECK(close_outcome.has_value());
}

// The waiting -> pending resume races the invocation end (issue #1165,
// dual-axis review): the waiting commit dirties the scheduler while the
// execution thread is still between that commit and ending its invocation,
// so the resume used to commit `pending` with a live identity — and the
// pending gate then under-reported the live phase. The scheduler now skips
// the resume until the identity is gone; this case pins the transition.
TEST_CASE("a waiting task is not resumed to pending while its phase is still invoked", "[durable][issue1165][resume]") {
    MemoryStorage storage;
    std::atomic_bool waiting_committed{false};
    std::atomic_bool release_phase{false};
    std::atomic_bool non_running_while_invoked{false};
    std::atomic_bool saw_pending_while_invoked{false};
    auto builder = TaskDefinitionBuilder("audit1165.resume", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase borrows the flags by reference and runs on a harness
    // execution thread; close() joins it before the flags leave scope.
    builder.phase("run",
            [&waiting_committed, &release_phase](
                    const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                if (waiting_committed.load()) {
                    // The scheduler resumed the parked wait: finish so close can drain.
                    return runtime.commit(
                            [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                                return NextTaskState::terminal(TaskOutcome{.status = "completed"});
                            });
                }
                auto committed = consume(runtime.commit(
                        [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            return NextTaskState::waiting(
                                    chord::Json(chord::Json::Object{{"phase", "parked"}}), {}, JoinPolicy::AllSettled);
                        }));
                if (!committed.has_value()) {
                    return support::AsyncResult<void>{std::unexpected(committed.error())};
                }
                waiting_committed.store(true);
                // Stay invoked well past the scheduler's dirty pass so the resume
                // race window is certainly observed.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!release_phase.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.resume"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());

    // Watch the invoked window: if the record flips back to `pending` while
    // the phase is active, inspect must still derive running (frozen pi
    // consults the identity first). Pre-fix this window produced a committed
    // `pending` record with a live identity and a non-running report.
    const auto watch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!saw_pending_while_invoked.load() && std::chrono::steady_clock::now() < watch_deadline) {
        if (!waiting_committed.load()) {
            std::this_thread::yield();
            continue;
        }
        auto record = storage.task(id);
        if (record && record->has_value() && (*record)->state.status == "pending") {
            saw_pending_while_invoked.store(true);
        }
        auto inspection = consume(harness->inspect());
        if (inspection.has_value()) {
            for (const auto& row : inspection.value().tasks) {
                if (row.record.id == id && row.state.kind != "running") {
                    non_running_while_invoked.store(true);
                }
            }
        }
        std::this_thread::yield();
    }
    release_phase.store(true);

    bool terminal = false;
    const auto terminal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!terminal && std::chrono::steady_clock::now() < terminal_deadline) {
        auto record = storage.task(id);
        if (record && record->has_value() && (*record)->state.status == "terminal") {
            terminal = true;
        }
        std::this_thread::yield();
    }
    const auto close_outcome = consume(harness->close());

    CHECK_FALSE(non_running_while_invoked.load());
    CHECK(terminal);
    CHECK(close_outcome.has_value());
}

// A reservation commit the Storage rejects must never publish a running
// identity and never start its phase: the commit failure seals the Session
// (issue #1166), and inspect rejects with the frozen failure message instead
// of scanning a record the scheduler has not actually reserved.
TEST_CASE("a rejected reservation never starts its phase or reports running", "[durable][issue1165][rejection]") {
    MemoryStorage memory;
    RejectReservationStorage storage(memory);
    std::atomic_bool phase_ready{false};
    auto builder = TaskDefinitionBuilder("audit1165.rejected", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase borrows the flag by reference and runs on a harness execution
    // thread; close() joins it before the flag leaves scope.
    builder.phase("run", [&phase_ready](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        phase_ready.store(true);
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.rejected"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());

    // The reservation's storage rejection fails the Session; inspect must
    // reject with the frozen seal rather than report a running identity, and
    // the rejected phase must never start (bounded).
    bool sealed = false;
    std::atomic_bool reported_running{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sealed && std::chrono::steady_clock::now() < deadline) {
        auto inspection = consume(harness->inspect());
        if (!inspection.has_value()) {
            if (inspection.error().message == "Session failed after a storage error; close and reopen it") {
                sealed = true;
            }
        } else {
            for (const auto& row : inspection.value().tasks) {
                if (row.record.id == id && row.state.kind == "running") {
                    reported_running.store(true);
                }
            }
        }
        std::this_thread::yield();
    }
    const auto close_outcome = consume(harness->close());

    CHECK(sealed);
    CHECK_FALSE(reported_running.load());
    CHECK_FALSE(phase_ready.load());
    CHECK(close_outcome.has_value());
}

// Terminal cleanup leaves no identity the inspection can mistake for running:
// once the terminal record settles, the task is not offered as live work even
// while its invocation's stop state is still registered, and no later inspect
// revives it.
TEST_CASE("a terminal task is never derived running from a live or stale identity", "[durable][issue1165][identity]") {
    MemoryStorage storage;
    std::atomic_bool terminal_committed{false};
    std::atomic_bool identity_still_live{false};
    std::atomic_bool violation{false};
    auto builder = TaskDefinitionBuilder("audit1165.identity", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase borrows the flags by reference and runs on a harness execution
    // thread; close() joins it before the flags leave scope.
    builder.phase("run",
            [&terminal_committed, &identity_still_live](
                    const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                auto committed = consume(runtime.commit(
                        [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
                        }));
                if (!committed.has_value()) {
                    return support::AsyncResult<void>{std::unexpected(committed.error())};
                }
                terminal_committed.store(true);
                // The invocation remains registered until the phase returns; hold that
                // window open so the inspection below races a live identity against a
                // terminal record.
                identity_still_live.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                identity_still_live.store(false);
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.identity"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());

    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!terminal_committed.load() && std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    // Inspect while the identity is still live with a terminal record, then
    // keep inspecting after cleanup, recording any live-work row.
    const auto observe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < observe_deadline) {
        auto inspection = consume(harness->inspect());
        if (inspection.has_value()) {
            for (const auto& row : inspection.value().tasks) {
                if (row.record.id == id) {
                    violation.store(true);
                }
            }
        }
        if (!identity_still_live.load() && terminal_committed.load()) break;
        std::this_thread::yield();
    }
    auto record = storage.task(id);
    const auto close_outcome = consume(harness->close());

    CHECK(terminal_committed.load());
    REQUIRE(record.has_value());
    REQUIRE(record->has_value());
    CHECK((*record)->state.status == "terminal");
    CHECK_FALSE(violation.load());
    CHECK(close_outcome.has_value());
}

// Held-commit lifecycle, release side (issue #1165 acceptance): once the line
// holder releases, the queued reservation settles, the phase starts on the
// published identity (inspect reports running while the record is committed
// running), and the identity cleans up when the phase settles the task
// terminal — no stale running row afterwards.
TEST_CASE("a held reservation settles after release: the phase starts and the identity cleans up",
        "[durable][issue1165][held-release]") {
    MemoryStorage storage;
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::atomic_bool observed_running{false};
    std::atomic_bool stale_identity{false};
    auto builder = TaskDefinitionBuilder("audit1165.held-release", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.held-release"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    // The holder borrows the flags and harness by reference and is joined
    // before they leave scope; its internal loop is bounded by `release`.
    std::jthread holder([&harness, &entered, &release] {
        static_cast<void>(consume(
                harness->session().read_committed([&entered, &release](Storage&) -> support::Expected<chord::Json> {
                    entered.store(true);
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    while (!release.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::yield();
                    return chord::Json(nullptr);
                })));
    });
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load() && std::chrono::steady_clock::now() < start_deadline)
        std::this_thread::yield();
    const bool did_enter = entered.load();
    REQUIRE(harness->resume().has_value());

    // While the commit is held the record stays pending with no running
    // report (the reservation is not yet settled); release and watch the
    // startup, then the cleanup.
    release.store(true);
    holder.join();

    bool terminal = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!terminal && std::chrono::steady_clock::now() < deadline) {
        auto record = storage.task(id);
        const bool record_terminal = record && record->has_value() && (*record)->state.status == "terminal";
        auto inspection = consume(harness->inspect());
        if (inspection.has_value()) {
            for (const auto& row : inspection.value().tasks) {
                if (row.record.id != id) {
                    continue;
                }
                if (record_terminal) {
                    stale_identity.store(true);
                } else if (row.record.state.status == "running" && row.state.kind == "running") {
                    observed_running.store(true);
                }
            }
        }
        if (record_terminal) {
            terminal = true;
        }
        std::this_thread::yield();
    }
    const auto close_outcome = consume(harness->close());

    CHECK(did_enter);
    CHECK(observed_running.load());
    CHECK(terminal);
    CHECK_FALSE(stale_identity.load());
    CHECK(close_outcome.has_value());
}

// Close race (issue #1165 acceptance): a reservation commit still queued
// behind a held Session line when shutdown begins must be discarded by its
// completion, never start its phase, and let close drain without a hang.
TEST_CASE("closing while a held reservation settles discards the identity and never starts the phase",
        "[durable][issue1165][close-race]") {
    MemoryStorage storage;
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::atomic_bool phase_ready{false};
    std::atomic_bool close_done{false};
    auto builder = TaskDefinitionBuilder("audit1165.close-race", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase borrows the flag by reference and runs on a harness execution
    // thread; close() joins it before the flag leaves scope.
    builder.phase("run", [&phase_ready](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        phase_ready.store(true);
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.close-race"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    // The holder and closer borrow the flags and harness by reference and are
    // joined before they leave scope; the holder's loop is bounded by
    // `release` and by its own deadline, so the joins below are reached.
    std::jthread holder([&harness, &entered, &release] {
        static_cast<void>(consume(
                harness->session().read_committed([&entered, &release](Storage&) -> support::Expected<chord::Json> {
                    entered.store(true);
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    while (!release.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::yield();
                    return chord::Json(nullptr);
                })));
    });
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load() && std::chrono::steady_clock::now() < start_deadline)
        std::this_thread::yield();
    const bool did_enter = entered.load();
    REQUIRE(harness->resume().has_value());
    std::jthread closer([&harness, &close_done] { close_done.store(consume(harness->close()).has_value()); });

    // Wait for the public scheduling state to flip to "closing" so the held
    // reservation's completion deterministically observes `closed` and
    // discards the identity.
    bool saw_closing = false;
    const auto closing_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!saw_closing && std::chrono::steady_clock::now() < closing_deadline) {
        auto inspection = consume(harness->inspect());
        if (inspection.has_value() && inspection.value().scheduling == "closing") {
            saw_closing = true;
        }
        std::this_thread::yield();
    }
    release.store(true);
    holder.join();
    closer.join();

    CHECK(did_enter);
    CHECK(saw_closing);
    CHECK_FALSE(phase_ready.load());
    CHECK(close_done.load());
}
