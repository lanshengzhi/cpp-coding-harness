#include <cch/durable/Harness.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>

#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
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

// The original visibility defect (issue #1165), fixed by the
// reservation-visibility slice. History: at the rebuild base the scheduler
// registered the invocation before its reservation commit settled, so an
// inspection whose scan observed the still-`pending` record derived
// `running` from a live identity (RED 10/10, log preserved under
// 1165-evidence/reservation-visibility-red-seam.log). Publication now
// happens only after settlement, so this case is green; the seam's line
// linearization also contributes here, and late publication's independent
// discrimination lives in [held-release], [rejection] and
// [pending-identity].
TEST_CASE("scheduler reservation is invisible to a read admitted before its commit",
        "[durable][issue1165][reservation-visibility]") {
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

    // The resume guard must keep a parked wait from flipping back to
    // `pending` while its phase is still invoked; the watch loop above
    // observes the window, so a regression surfaces here.
    CHECK_FALSE(saw_pending_while_invoked.load());
    CHECK_FALSE(non_running_while_invoked.load());
    CHECK(terminal);
    CHECK(close_outcome.has_value());
}

// A reservation commit the Storage rejects must never publish a running
// identity and never start its phase: the commit failure seals the Session
// (issue #1166), and inspect rejects with the frozen failure message instead
// of scanning a record the scheduler has not actually reserved.
// The rejected-reservation contract survives the rebuild: a reservation
// whose Storage write fails seals the Session (#1166), the phase never
// starts, and inspect never reports a running identity. It passes again now
// that publication happens in the reservation completion (issue #1165).
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

namespace {
/// Holds one task kind's reservation commit (`pending` -> `running`) inside
/// `Storage::commit`. The gate firing proves the reservation commit was
/// admitted by the Session line (the write is executing inside the commit
/// turn) while Storage settlement is still blocked, which is the exact
/// asynchronous-reservation interleaving issue #1165 pins. Thread-safe: the
/// flag state is atomic and the gate blocks only its own call.
class HeldReservationStorage final : public Storage {
public:
    explicit HeldReservationStorage(Storage& inner) : inner_(inner) {}

    void arm() noexcept { armed.store(true, std::memory_order_release); }
    [[nodiscard]] bool commit_seen() const noexcept { return commit_seen_.load(std::memory_order_acquire); }
    void release() noexcept { released.store(true, std::memory_order_release); }
    /// Fail the held commit (instead of letting it delegate) once released.
    void fail_on_release() noexcept { fail_on_release_.store(true, std::memory_order_release); }

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        bool holds_reservation = false;
        if (armed.load(std::memory_order_acquire)) {
            for (const auto& write : writes) {
                if (write.type == "task" && write.task.has_value() && write.task->kind == "audit1165.held" &&
                        write.task->state.status == "running") {
                    holds_reservation = true;
                }
            }
        }
        if (holds_reservation) {
            commit_seen_.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!released.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            if (fail_on_release_.load(std::memory_order_acquire)) {
                return support::AsyncResult<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Process, "injected held-commit failure"))};
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
    std::atomic_bool armed{false};
    std::atomic_bool commit_seen_{false};
    std::atomic_bool released{false};
    std::atomic_bool fail_on_release_{false};
};

/// Event-order recorder for issue #1165 admission-ordered inspect evidence.
/// Wraps a Storage and appends every observed task-write sighting, task-write
/// completion (after delegation), scan entry, and gate release to one
/// mutex-protected sequence. Tests assert pure event ORDER plus outcome
/// content -- no started-flags, timers, or mutex-fairness assumptions. A
/// configured create kind can be held until released, like
/// HeldReservationStorage.
class SequenceRecordingStorage final : public Storage {
public:
    explicit SequenceRecordingStorage(Storage& inner) : inner_(inner) {}

    void hold_kind(std::string kind) { hold_kind_ = std::move(kind); }
    void arm() noexcept { armed.store(true, std::memory_order_release); }
    void release() noexcept {
        record("released");
        released.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool w_seen(std::string_view kind) const {
        std::lock_guard lock(events_mutex_);
        for (const auto& event : events_) {
            if (event.rfind("w:" + std::string(kind), 0) == 0) return true;
        }
        return false;
    }
    [[nodiscard]] std::size_t event_index(std::string_view event) const {
        std::lock_guard lock(events_mutex_);
        for (std::size_t i = 0; i < events_.size(); ++i) {
            if (events_[i] == event) return i;
        }
        return npos;
    }
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        bool holds = false;
        std::string held_kind;
        if (armed.load(std::memory_order_acquire)) {
            for (const auto& write : writes) {
                if (write.type == "task" && write.task.has_value() && write.task->kind == hold_kind_ &&
                        write.task->state.status == "running") {
                    holds = true;
                    held_kind = write.task->kind;
                }
            }
        }
        if (holds) {
            record("w:" + held_kind + ":running");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!released.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            auto result = inner_.commit(writes);
            record("w-done:" + held_kind);
            return result;
        }
        for (const auto& write : writes) {
            if (write.type == "task" && write.task.has_value())
                record("w:" + write.task->kind + ":" + write.task->state.status);
        }
        return inner_.commit(writes);
    }

    [[nodiscard]] support::Expected<std::vector<TaskRecord>> scan_tasks(std::size_t limit) override {
        record("scan");
        return inner_.scan_tasks(limit);
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
    void record(std::string event) const {
        std::lock_guard lock(events_mutex_);
        events_.push_back(std::move(event));
    }

    Storage& inner_; // outlives this wrapper
    std::string hold_kind_;
    std::atomic_bool armed{false};
    std::atomic_bool released{false};
    mutable std::mutex events_mutex_;
    mutable std::vector<std::string> events_;
};

/// Models a slow-read Storage backend: after arming, every `scan_tasks`
/// call (limit <= 256, the scheduler/inspect scan size) whose result
/// contains the target kind still `waiting` observes the live rows, then
/// blocks with that observation in hand until released, and finally returns
/// it. A real backend whose scan observed at T1 returns at T2 behaves the
/// same way. Thread-safe; the blocked callers are attributed by thread id so
/// a test can wait for a specific caller's scan to be held.
class SlowScanStorage final : public Storage {
public:
    explicit SlowScanStorage(Storage& inner, std::string kind, std::string status = "waiting")
        : inner_(inner), kind_(std::move(kind)), status_(std::move(status)) {}

    void arm() noexcept { armed.store(true, std::memory_order_release); }
    void release() noexcept { released.store(true, std::memory_order_release); }

    /// Whether the scan issued by `tid` is currently blocked in the gate.
    [[nodiscard]] bool held_by(std::thread::id tid) {
        std::lock_guard lock(held_mutex_);
        return held_.contains(tid);
    }

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
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
        auto observed = inner_.scan_tasks(limit);
        bool holdable = armed.load(std::memory_order_acquire) && !released.load(std::memory_order_acquire) &&
                        limit <= 256 && observed.has_value();
        if (holdable) {
            holdable = std::any_of(observed->begin(), observed->end(), [&](const TaskRecord& record) {
                return record.kind == kind_ && record.state.status == status_;
            });
        }
        if (!holdable) {
            return observed;
        }
        const auto tid = std::this_thread::get_id();
        {
            std::lock_guard lock(held_mutex_);
            held_.insert(tid);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!released.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        {
            std::lock_guard lock(held_mutex_);
            held_.erase(tid);
        }
        return observed;
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
    std::string kind_;
    std::string status_;
    std::atomic_bool armed{false};
    std::atomic_bool released{false};
    std::mutex held_mutex_;
    std::set<std::thread::id> held_;
};
} // namespace

// Held asynchronous reservation, release side (issue #1165 acceptance). The
// gate blocks the reservation commit inside Storage::commit: the commit is
// admitted (proven by `commit_seen`) but unsettled. While the turn is held
// the Session line is blocked, and inspect's seal check
// (`Session::failed()`) waits on the line, so an inspect started during the
// hold cannot return before the reservation settles -- the same FIFO wait a
// frozen pi `readOnLine` read would spend behind the held commit turn. After
// the release the settled reservation is truthfully visible: the record is
// committed running, inspect reports running, and the phase (gated so the
// transient cannot be missed) starts on the published identity and cleans it
// up when it settles the task terminal.
TEST_CASE("a held reservation commit stays unreserved until it settles", "[durable][issue1165][held-release]") {
    MemoryStorage memory;
    HeldReservationStorage storage(memory);
    std::atomic_bool release_phase{false};
    std::atomic_bool observed_running{false};
    std::atomic_bool stale_identity{false};
    auto builder = TaskDefinitionBuilder("audit1165.held", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase borrows the flag by reference and runs on a harness execution
    // thread; close() joins it before the flag leaves scope.
    builder.phase("run", [&release_phase](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        // Stay invoked until the observer has seen the running row, so the
        // transient running state cannot be missed on a correct scheduler.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!release_phase.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
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
    REQUIRE(consume(harness->commit([&harness, &id](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.held"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    storage.arm();
    REQUIRE(harness->resume().has_value());

    // The reservation commit is admitted but held: the gate firing inside
    // Storage::commit proves the commit turn is executing, and the record
    // cannot advance past pending until the gate releases.
    const auto admitted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!storage.commit_seen() && std::chrono::steady_clock::now() < admitted_deadline)
        std::this_thread::yield();
    REQUIRE(storage.commit_seen());
    auto pending_record = memory.task(id);
    REQUIRE(pending_record.has_value());
    REQUIRE(pending_record->has_value());
    REQUIRE((*pending_record)->state.status == "pending");

    // An inspect started during the hold waits on the blocked line (the seal
    // check takes the line mutex) and must not return while the reservation
    // is unsettled.
    std::atomic_bool inspect_started{false};
    std::atomic_bool inspect_returned{false};
    std::atomic_bool held_row_running{false};
    std::jthread inspector([&storage, &harness, &id, &inspect_started, &inspect_returned, &held_row_running] {
        inspect_started.store(true);
        auto inspection = consume(harness->inspect());
        inspect_returned.store(true);
        if (!inspection.has_value()) {
            return;
        }
        for (const auto& row : inspection.value().tasks) {
            if (row.record.id == id && row.state.kind == "running") {
                held_row_running.store(true);
            }
        }
        static_cast<void>(storage);
    });
    const auto started_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!inspect_started.load() && std::chrono::steady_clock::now() < started_deadline)
        std::this_thread::yield();
    REQUIRE(inspect_started.load());
    const auto blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!inspect_returned.load() && std::chrono::steady_clock::now() < blocked_deadline)
        std::this_thread::yield();
    const bool inspect_waited = !inspect_returned.load();
    REQUIRE(inspect_waited);

    // Release: the reservation settles, the completion publishes the
    // identity, and the blocked inspect observes the settled state. The
    // phase then runs on the published identity until the observer's own
    // inspect has seen the running row.
    storage.release();
    inspector.join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool terminal = false;
    while (!terminal && std::chrono::steady_clock::now() < deadline) {
        auto record = memory.task(id);
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
                    release_phase.store(true);
                }
            }
        }
        if (record_terminal && observed_running.load()) {
            terminal = true;
        }
        std::this_thread::yield();
    }
    const auto close_outcome = consume(harness->close());

    CHECK(held_row_running.load());
    CHECK(observed_running.load());
    CHECK(terminal);
    CHECK_FALSE(stale_identity.load());
    CHECK(close_outcome.has_value());
}

// Held asynchronous reservation, close side (issue #1165 acceptance). While
// the reservation commit is admitted but held, shutdown begins and sets the
// scheduler's `closed` flag long before the held turn can settle (shutdown
// blocks in the worker join, which waits on the unsettled reservation). The
// gate then releases: the reservation write settles, its completion observes
// `closed` and discards the identity, the phase never starts, and close
// drains without a hang. Ordering the release after the `closed` flag relies
// on a bounded margin, not a proof: the `closing_started` signal precedes the
// flag by two uncontended critical sections (registry unsubscribe, Harness
// mutex), and a bounded yield window (~10^5x that cost) separates them. The
// post-release assertions are exact; the setup ordering itself is by margin,
// so this case pins the discard semantics rather than claiming to prove the
// preemption-impossible.
TEST_CASE("closing with a held reservation commit discards the identity and never starts the phase",
        "[durable][issue1165][close-race]") {
    MemoryStorage memory;
    HeldReservationStorage storage(memory);
    std::atomic_bool phase_ready{false};
    std::atomic_bool close_done{false};
    std::atomic_bool closing_started{false};
    auto builder = TaskDefinitionBuilder("audit1165.held", 1);
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
    REQUIRE(consume(harness->commit([&harness, &id](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.held"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    storage.arm();
    REQUIRE(harness->resume().has_value());
    const auto admitted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!storage.commit_seen() && std::chrono::steady_clock::now() < admitted_deadline)
        std::this_thread::yield();
    REQUIRE(storage.commit_seen());
    auto pending_record = memory.task(id);
    REQUIRE(pending_record.has_value());
    REQUIRE(pending_record->has_value());
    REQUIRE((*pending_record)->state.status == "pending");

    // Begin shutdown while the reservation commit is held. Shutdown sets the
    // `closed` flag before joining the worker, and the join cannot pass until
    // the reservation settles -- which only happens at the release below.
    std::jthread closer([&harness, &close_done, &closing_started] {
        closing_started.store(true);
        close_done.store(consume(harness->close()).has_value());
    });
    const auto started_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!closing_started.load() && std::chrono::steady_clock::now() < started_deadline)
        std::this_thread::yield();
    REQUIRE(closing_started.load());
    const auto flag_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < flag_deadline)
        std::this_thread::yield();
    storage.release();
    closer.join();

    CHECK(close_done.load());
    CHECK_FALSE(phase_ready.load());
}

// Stale-scan linearization (issue #1165 review): with a slow-read backend,
// inspect's scan observes a waiting record whose invocation is active, the
// invocation cleans up and the task terminalizes while the scan is blocked,
// and the identity snapshot is taken after the release. Deriving a non-running
// state from the pre-cleanup record then reports a state that never existed
// at any instant: the record was waiting with a live invocation, or terminal
// with none. Frozen pi reads the whole inspection on the Session line
// (`readOnLine`), where no such interleaving exists; this case pins the same
// public behavior for the direct-scan inspection.
// HISTORICAL direct-scan RED, kept as the record of the pre-seam defect. It
// must not run against the line-read seam: the held scan now holds the line,
// so the test's own terminal commit can only queue behind it and can never
// settle before storage.release() -- the test would deadlock by construction.
// The seam-era property is pinned by [stale-scan-line] instead.
TEST_CASE(
        "a slow scan does not pair a pre-cleanup record with a post-cleanup identity view", "[.][stale-scan-legacy]") {
    MemoryStorage memory;
    SlowScanStorage storage(memory, "audit1165.stale-scan");
    std::atomic_bool waiting_committed{false};
    std::atomic_bool release_phase{false};
    std::atomic_bool phase_returned{false};
    auto builder = TaskDefinitionBuilder("audit1165.stale-scan", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    // The phase borrows the flags by reference and runs on a harness
    // execution thread; close() joins it before the flags leave scope. The
    // scheduler resumes a parked wait only after this invocation ends, so the
    // terminal record below is committed by the test itself.
    builder.phase("run",
            [&waiting_committed, &release_phase, &phase_returned](
                    const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                auto committed = consume(runtime.commit(
                        [](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                            return NextTaskState::waiting(
                                    chord::Json(chord::Json::Object{{"phase", "parked"}}), {}, JoinPolicy::AllSettled);
                        }));
                if (!committed.has_value()) {
                    return support::AsyncResult<void>{std::unexpected(committed.error())};
                }
                waiting_committed.store(true);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!release_phase.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                phase_returned.store(true);
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
    REQUIRE(consume(harness->commit([&harness, &id](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.stale-scan"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    const auto waiting_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waiting_committed.load() && std::chrono::steady_clock::now() < waiting_deadline)
        std::this_thread::yield();
    REQUIRE(waiting_committed.load());

    // Arm the gate from the inspector thread, run inspect there, and wait
    // until that thread's scan is verifiably held with its waiting
    // observation in hand. (The scheduler's own scans may be held too; the
    // attribution keeps this wait independent of scheduling delays.)
    std::atomic_bool row_violation{false};
    std::atomic_bool saw_row{false};
    std::thread::id inspector_tid{};
    std::mutex tid_mutex;
    std::condition_variable tid_cv;
    bool tid_ready = false;
    std::jthread inspector(
            [&storage, &harness, &id, &row_violation, &saw_row, &inspector_tid, &tid_mutex, &tid_cv, &tid_ready] {
                {
                    std::lock_guard lock(tid_mutex);
                    inspector_tid = std::this_thread::get_id();
                    tid_ready = true;
                }
                tid_cv.notify_all();
                storage.arm();
                auto inspection = consume(harness->inspect());
                if (!inspection.has_value()) {
                    return;
                }
                for (const auto& row : inspection.value().tasks) {
                    if (row.record.id == id) {
                        saw_row.store(true);
                        if (row.record.state.status == "waiting" && row.state.kind != "running") {
                            row_violation.store(true);
                        }
                    }
                }
            });
    {
        std::unique_lock lock(tid_mutex);
        tid_cv.wait(lock, [&tid_ready] { return tid_ready; });
    }
    const auto held_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!storage.held_by(inspector_tid) && std::chrono::steady_clock::now() < held_deadline)
        std::this_thread::yield();
    REQUIRE(storage.held_by(inspector_tid));

    // While the scan is held, the phase returns and its invocation cleans up;
    // then the test settles the task terminal. The released inspection pairs
    // the pre-cleanup record observation with a post-cleanup identity view.
    release_phase.store(true);
    const auto returned_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!phase_returned.load() && std::chrono::steady_clock::now() < returned_deadline)
        std::this_thread::yield();
    REQUIRE(phase_returned.load());
    REQUIRE(consume(harness->commit([id](Transaction& tx) -> support::ExpectedVoid {
        auto cur = tx.task(id);
        if (!cur || !cur->has_value()) return {};
        TaskRecord record = **cur;
        if (record.state.status != "waiting") return {};
        record.state.status = "terminal";
        record.state.outcome = TaskOutcome{.status = "completed"};
        return tx.set_task(std::move(record));
    })).has_value());
    bool terminal = false;
    const auto terminal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!terminal && std::chrono::steady_clock::now() < terminal_deadline) {
        auto record = memory.task(id);
        if (record && record->has_value() && (*record)->state.status == "terminal") {
            terminal = true;
        }
        std::this_thread::yield();
    }
    REQUIRE(terminal);
    storage.release();
    inspector.join();
    const auto close_outcome = consume(harness->close());

    CHECK_FALSE(row_violation.load());
    CHECK(close_outcome.has_value());
    static_cast<void>(saw_row);
}

namespace {
[[nodiscard]] TaskDefinition make_terminal_task(std::string kind) {
    auto builder = TaskDefinitionBuilder(std::move(kind), 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    return std::move(*definition);
}

[[nodiscard]] TaskDefinition make_parked_task(std::string kind, const std::atomic_bool& park) {
    auto builder = TaskDefinitionBuilder(std::move(kind), 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&park](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!park.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "completed"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    return std::move(*definition);
}

[[nodiscard]] bool create_task_by_kind(Harness& harness, std::string_view kind) {
    return consume(harness.commit([&](Transaction& tx) -> support::ExpectedVoid {
        const auto created = tx.create_task(*harness.task_definition(kind), chord::Json(nullptr));
        if (!created) return std::unexpected(created.error());
        return {};
    })).has_value();
}

[[nodiscard]] bool inspection_has_row(const HarnessInspection& inspection, std::string_view kind) {
    for (const auto& row : inspection.tasks) {
        if (row.record.kind == kind) return true;
    }
    return false;
}

[[nodiscard]] std::optional<std::string> inspection_row_state(
        const HarnessInspection& inspection, std::string_view kind) {
    for (const auto& row : inspection.tasks) {
        if (row.record.kind == kind) return row.state.kind;
    }
    return std::nullopt;
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate&& predicate, std::chrono::seconds budget = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return predicate();
}
} // namespace

// Included-set definition (issue #1165 protocol work, design doc section 6).
// Evidence is pure event ORDER from SequenceRecordingStorage plus outcome
// content -- no started-flags, timers, or mutex-fairness assumptions. What
// Storage can observe is the read's capture (its scan), not its Session-line
// enqueue: a read leaves no storage trace before it runs. The property under
// test is therefore stated at the capture instant, which is observable:
//   reviewed protocol: w:held:running < w-done:held < scan < w:fifo-b:*
//   (the read job runs after the held turn fully settles and before the
//    later create's turn executes), and the inspection includes the held
//    task's effects while excluding the later create.
//   today:             w:held:running < scan < released
//   (the direct scan runs while the held turn is still unsettled), and the
//    inspection lacks the held task's effects -- the undefined included set.
// Design-constraint test (issue #1165): from the PUBLIC seam the Session-line
// admission of a read is unobservable -- a read leaves no storage trace until
// it runs -- and today's capture instant is bimodal (the held turn sometimes
// holds the line lock during its storage flight, sometimes not), so this test
// cannot be a deterministic RED today. It is written for the reviewed
// protocol's world (queued read -> capture deterministically after the held
// turn settles) and hidden until the private seam lands; the suite's
// deterministic REDs are [stale-scan] and [closed-admission]. See
// 1165-evidence/fifo-order-admission-evidence.log for the recorded modes.
TEST_CASE("scheduler inspect has a defined admission-ordered included set", "[.][fifo-order-probe]") {
    MemoryStorage memory;
    SequenceRecordingStorage storage{memory};
    storage.hold_kind("audit1165.held");
    std::atomic_bool unpark{false};
    std::vector<TaskDefinition> definitions;
    definitions.push_back(make_parked_task("audit1165.held", unpark));
    definitions.push_back(make_terminal_task("audit1165.fifo-b"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());

    REQUIRE(create_task_by_kind(*harness, "audit1165.held"));
    storage.arm();
    REQUIRE(harness->resume().has_value());
    const bool held_admitted = wait_until([&storage] { return storage.w_seen("audit1165.held"); });
    if (!held_admitted) storage.release(); // keep teardown off a stranded worker on the failure path
    REQUIRE(held_admitted);

    std::optional<Outcome<HarnessInspection>> outcome;
    std::thread inspect_thread{[&] { outcome = consume(harness->inspect()); }};
    // Event-ordered world detection, not timing: today's direct scan always
    // reaches Storage during the hold; under the reviewed protocol the read
    // job is queued behind the held turn and cannot scan before the release.
    const bool scan_during_hold =
            wait_until([&storage] { return storage.event_index("scan") != SequenceRecordingStorage::npos; },
                    std::chrono::seconds(2));
    const bool later_create_enqueued = create_task_by_kind(*harness, "audit1165.fifo-b");
    storage.release();
    if (inspect_thread.joinable()) inspect_thread.join();
    const bool follow_up_ok = [&] {
        auto later = consume(harness->inspect());
        return later.has_value() && inspection_has_row(later.value(), "audit1165.held") &&
               inspection_has_row(later.value(), "audit1165.fifo-b");
    }();
    const bool close_outcome = consume(harness->close()).has_value();

    const auto scan_at = storage.event_index("scan");
    const auto held_done_at = storage.event_index("w-done:audit1165.held");
    const auto released_at = storage.event_index("released");
    const auto later_at = storage.event_index("w:audit1165.fifo-b:waiting");
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    const auto held_state = inspection_row_state(outcome->value(), "audit1165.held");
    REQUIRE(held_state.has_value());
    CHECK_FALSE(inspection_has_row(outcome->value(), "audit1165.fifo-b"));
    if (scan_during_hold) {
        // Today's direct scan: the capture demonstrably ran mid-hold (the
        // scan event preceded the release and the held turn's completion),
        // so the row cannot carry the reservation's running effect.
        CHECK(scan_at < released_at);
        CHECK(*held_state != "running");
    } else {
        // The reviewed protocol: the capture ran inside a line job after the
        // held turn fully settled (so the parked phase reads running) and
        // before the later create's turn.
        CHECK(held_done_at != SequenceRecordingStorage::npos);
        CHECK(held_done_at < scan_at);
        CHECK((later_at == SequenceRecordingStorage::npos || scan_at < later_at));
        CHECK(*held_state == "running");
    }
    unpark.store(true, std::memory_order_release);
}

// Seal delivery (issue #1166 boundary): an inspection consumed while a held
// commit is in flight must observe the session seal when that commit's
// storage write fails. Guards the reject/drain path for a read that was
// already waiting when the failure landed.
TEST_CASE(
        "scheduler inspect consumed before a failure observes the session seal", "[durable][issue1165][seal-waiting]") {
    MemoryStorage memory;
    HeldReservationStorage storage{memory};
    std::vector<TaskDefinition> definitions;
    definitions.push_back(make_terminal_task("audit1165.held"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());

    REQUIRE(create_task_by_kind(*harness, "audit1165.held"));
    storage.arm();
    REQUIRE(harness->resume().has_value());
    const bool reservation_admitted = wait_until([&storage] { return storage.commit_seen(); });
    if (!reservation_admitted) storage.release(); // keep teardown off a stranded worker on the failure path
    REQUIRE(reservation_admitted);
    storage.fail_on_release();

    std::optional<Outcome<HarnessInspection>> outcome;
    std::thread inspect_thread{[&] { outcome = consume(harness->inspect()); }};
    const auto held_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (outcome.has_value() == false && std::chrono::steady_clock::now() < held_wait_deadline)
        std::this_thread::yield();
    storage.release();
    if (inspect_thread.joinable()) inspect_thread.join();
    const bool close_outcome = consume(harness->close()).has_value();
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().message == "Session failed after a storage error; close and reopen it");
    const bool sealed_after = [&] {
        auto again = consume(harness->inspect());
        return !again.has_value();
    }();
    CHECK(close_outcome);
    CHECK(sealed_after);
}

// Close boundary (issue #1165 protocol work): an inspection consumed while a
// commit turn is held must not outlive close() only to fail inside a closed
// storage. Either close's drain runs it (a value) or admission rejects it
// with the defined closed error. NOT verification: today's pass is racy (the
// capture usually wins the ordering against close's storage teardown), so
// this is a contract target for the reviewed protocol, not evidence.
TEST_CASE("scheduler inspect does not fail inside closed storage after close", "[durable][issue1165][close-drain]") {
    MemoryStorage memory;
    HeldReservationStorage storage{memory};
    std::vector<TaskDefinition> definitions;
    definitions.push_back(make_terminal_task("audit1165.held"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());

    REQUIRE(create_task_by_kind(*harness, "audit1165.held"));
    storage.arm();
    REQUIRE(harness->resume().has_value());
    const bool reservation_admitted = wait_until([&storage] { return storage.commit_seen(); });
    if (!reservation_admitted) storage.release(); // keep teardown off a stranded worker on the failure path
    REQUIRE(reservation_admitted);

    std::optional<Outcome<HarnessInspection>> outcome;
    std::thread inspect_thread{[&] { outcome = consume(harness->inspect()); }};
    std::optional<bool> close_outcome;
    std::thread close_thread{[&] { close_outcome = consume(harness->close()).has_value(); }};
    const auto held_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (close_outcome.has_value() == false && std::chrono::steady_clock::now() < held_wait_deadline)
        std::this_thread::yield();
    storage.release();
    if (inspect_thread.joinable()) inspect_thread.join();
    if (close_thread.joinable()) close_thread.join();
    REQUIRE(outcome.has_value());
    // Contract: ran during close's drain (a value) or rejected at/after close
    // with the defined closed error -- never a raw storage error.
    const bool defined = outcome->has_value() || outcome->error().message == "Session is closed";
    CHECK(defined);
    REQUIRE(close_outcome.has_value());
    CHECK(*close_outcome);
}

// Closed-admission parity: after close(), inspect must reject with the
// frozen `assertUsable` message ("Session is closed") instead of whatever
// the storage backend reports from its closed state.
TEST_CASE("scheduler inspect after close reports the defined closed error", "[durable][issue1165][closed-admission]") {
    MemoryStorage storage;
    std::vector<TaskDefinition> definitions;
    definitions.push_back(make_terminal_task("audit1165.held"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());

    REQUIRE(consume(harness->close()).has_value());
    auto outcome = consume(harness->inspect());
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "Session is closed");
}

// The definition-replacement path commits `running -> pending` and only then
// erases the invocation identity (Harness.cpp run_task_execution). In that
// window a committed `pending` record coexists with a live identity, and a
// pending record must never be promoted to running by identity alone. The
// same state is constructed deterministically here: the parked phase keeps
// its identity, and a public commit writes `pending`.
TEST_CASE("a committed pending record is never promoted by a live identity", "[durable][issue1165][pending-identity]") {
    MemoryStorage storage;
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    auto builder = TaskDefinitionBuilder("audit1165.pending-identity", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&entered, &release](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!release.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
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
    REQUIRE(consume(harness->commit([&harness, &id](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.pending-identity"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_until([&entered] { return entered.load(); }));
    // The phase is parked: its identity is live and the record is `running`.
    // Write `pending` directly, reproducing the replacement window.
    REQUIRE(consume(harness->commit([id](Transaction& tx) -> support::ExpectedVoid {
        auto cur = tx.task(id);
        if (!cur || !cur->has_value()) return {};
        TaskRecord record = **cur;
        if (record.state.status != "running") return {};
        record.state.status = "pending";
        return tx.set_task(std::move(record));
    })).has_value());
    const auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    const auto state = inspection_row_state(inspection.value(), "audit1165.pending-identity");
    REQUIRE(state.has_value());
    CHECK(*state == "ready");
    release.store(true);
    CHECK(consume(harness->close()).has_value());
}

// A committed `running` record reports running even when no invocation is
// live: the identity is not the only running source (frozen `#inspectTask`
// checks the invocation first and then the record's running status). Without
// the record rule, a phase that ended abnormally -- or a crash-recovered
// record -- would be reported ready.
TEST_CASE(
        "a committed running record reports running without a live identity", "[durable][issue1165][running-record]") {
    MemoryStorage storage;
    std::vector<TaskDefinition> definitions;
    definitions.push_back(make_terminal_task("audit1165.running-record"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&harness, &id](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.running-record"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    // The harness stays paused: the record is committed `running` directly,
    // with no invocation in flight and no identity to consult.
    REQUIRE(consume(harness->commit([id](Transaction& tx) -> support::ExpectedVoid {
        auto cur = tx.task(id);
        if (!cur || !cur->has_value()) return {};
        TaskRecord record = **cur;
        record.state.status = "running";
        return tx.set_task(std::move(record));
    })).has_value());
    const auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    const auto state = inspection_row_state(inspection.value(), "audit1165.running-record");
    REQUIRE(state.has_value());
    CHECK(*state == "running");
    CHECK(consume(harness->close()).has_value());
}

// Seam-era coherence (issue #1165): while the inspection's scan is held
// inside its line job, a terminal commit started on another thread must be
// line-blocked -- it cannot publish while the inspection owns the line --
// and after the release the inspection's own row still reflects the record
// it observed, while the raw record settles terminal. A paused harness keeps
// the record stably `pending`, so no scheduler activity races the scan.
// Release paths precede every join and assertion; every wait is bounded.
TEST_CASE("a held inspection scan blocks a later terminal commit until it settles",
        "[durable][issue1165][stale-scan-line]") {
    MemoryStorage memory;
    SlowScanStorage storage(memory, "audit1165.stale-line", "pending");
    std::vector<TaskDefinition> definitions;
    definitions.push_back(make_terminal_task("audit1165.stale-line"));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&harness, &id](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.stale-line"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    // The harness stays paused: the record is `pending` and no invocation
    // exists, so the gated scan observes a stable record.

    std::atomic_bool row_violation{false};
    std::atomic_bool saw_row{false};
    std::thread::id inspector_tid{};
    std::mutex tid_mutex;
    std::condition_variable tid_cv;
    bool tid_ready = false;
    std::jthread inspector(
            [&storage, &harness, &id, &row_violation, &saw_row, &inspector_tid, &tid_mutex, &tid_cv, &tid_ready] {
                {
                    std::lock_guard lock(tid_mutex);
                    inspector_tid = std::this_thread::get_id();
                    tid_ready = true;
                }
                tid_cv.notify_all();
                storage.arm();
                auto inspection = consume(harness->inspect());
                if (!inspection.has_value()) {
                    return;
                }
                for (const auto& row : inspection.value().tasks) {
                    if (row.record.id == id) {
                        saw_row.store(true);
                        // The gated scan observed the pending record; the line-held
                        // terminal commit cannot have published yet.
                        if (row.record.state.status != "pending") {
                            row_violation.store(true);
                        }
                    }
                }
            });
    {
        std::unique_lock lock(tid_mutex);
        tid_cv.wait(lock, [&tid_ready] { return tid_ready; });
    }
    const auto held_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!storage.held_by(inspector_tid) && std::chrono::steady_clock::now() < held_deadline)
        std::this_thread::yield();
    const bool scan_held = storage.held_by(inspector_tid);

    // A terminal commit from another thread must queue behind the held
    // inspection and stay unpublished while the scan is held.
    std::optional<bool> commit_settled;
    std::thread committer{[&harness, &commit_settled, id] {
        commit_settled = consume(harness->commit([id](Transaction& tx) -> support::ExpectedVoid {
            auto cur = tx.task(id);
            if (!cur || !cur->has_value()) return {};
            TaskRecord record = **cur;
            if (record.state.status != "pending") return {};
            record.state.status = "terminal";
            record.state.outcome = TaskOutcome{.status = "completed"};
            return tx.set_task(std::move(record));
        })).has_value();
    }};
    const auto blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!commit_settled.has_value() && std::chrono::steady_clock::now() < blocked_deadline)
        std::this_thread::yield();
    const bool commit_blocked = !commit_settled.has_value();
    const auto record_while_held = memory.task(id);

    storage.release();
    if (committer.joinable()) committer.join();
    if (inspector.joinable()) inspector.join();
    const auto final_record = memory.task(id);
    const bool close_outcome = consume(harness->close()).has_value();

    REQUIRE(scan_held);
    CHECK(commit_blocked);
    REQUIRE(record_while_held.has_value());
    CHECK(record_while_held->has_value());
    CHECK((*record_while_held)->state.status == "pending");
    CHECK(saw_row.load());
    CHECK_FALSE(row_violation.load());
    REQUIRE(commit_settled.has_value());
    CHECK(*commit_settled);
    REQUIRE(final_record.has_value());
    CHECK(final_record->has_value());
    CHECK((*final_record)->state.status == "terminal");
    CHECK(close_outcome);
}
