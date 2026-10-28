#include <cch/durable/Harness.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>

#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

/// Rejects one specifically tagged public Harness entry commit, mirroring the
/// frozen failure probe's `FailStorage` (`/tmp/cch-1088-frozen-failure.ts`).
/// The injection decision is a pure function of the write batch: the tag and
/// the reject observer are immutable after construction, so the double is
/// deterministic, narrowly targeted, and safe under the Session line's
/// serial commit admission (no mutable cross-thread latch).
class TaggedFailureStorage final : public Storage {
public:
    TaggedFailureStorage(Storage& inner, std::string rejected_entry_kind, std::function<void()> on_reject)
        : inner_(inner), rejected_entry_kind_(std::move(rejected_entry_kind)), on_reject_(std::move(on_reject)) {}

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        for (const auto& write : writes) {
            if (write.entry && write.entry->kind == rejected_entry_kind_) {
                if (on_reject_) on_reject_();
                return support::AsyncResult<Seq>{std::unexpected(
                        support::make_error(support::ErrorCode::Process, "injected 1166 failure"))};
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
    std::string rejected_entry_kind_;
    std::function<void()> on_reject_;
};

[[nodiscard]] std::vector<TaskDefinition> failure_task_definitions() {
    // Fresh builders per Harness: a definition token is move-only identity.
    auto builder = TaskDefinitionBuilder("audit1166.failure", 1);
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
    return definitions;
}

[[nodiscard]] bool wait_for(const std::atomic_bool& flag, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return flag.load();
}

} // namespace

// Frozen authority: the frozen public failure probe (pi-v1.3.0
// `71023aec1546d6d677a65669c5665c0927ce0dd0`, repeated 3/3) observes
// ["phase","fail-call","storage-reject","abort-enter","abort-return",
//  "inspect-rejected","commit-rejected"]: the Session fails, the close
// notification aborts every active invocation, and only then does the failing
// commit's caller observe the rejection. The frozen scheduler delivers the
// abort in `beforeClose -> TaskScheduler.join`, before the failure reaches
// the caller.
//
// Discriminating case: a Harness that returns the storage error without
// signalling the active invocation first (issue1165 main-probe RED,
// /tmp/cch-1165-main-failure-red.log) fails `signalled_before_failure_return`,
// the frozen-order event equality, and the sealing checks below.
//
// Honest scope: on frozen pi the handler's inspect settles after
// `abort-return` (the probe queues it asynchronously; a microtask drains it
// before the commit rejection propagates). Here the stop callback runs on the
// settlement thread, and the sealed Session answers the admission check
// inline, so `inspect-rejected` lands between `abort-enter` and
// `abort-return`. The frozen-required properties hold exactly: the signal is
// delivered before the failing commit returns, the Session is sealed for
// every later operation, and the caller still receives the original storage
// error.
TEST_CASE("a storage failure signals the active invocation before the failing commit returns",
        "[durable][issue1166][failure][spec]") {
    MemoryStorage memory;
    std::vector<std::string> events;
    const auto record_event = [&events](std::string name) { events.push_back(std::move(name)); };
    TaggedFailureStorage storage(memory, "audit1166.fail", [&] { record_event("storage-reject"); });

    std::atomic_bool phase_ready{false};
    std::atomic_bool release_phase{false};
    std::atomic_bool phase_done{false};
    std::atomic_bool stop_observed_by_phase{false};
    std::atomic_bool signalled{false};
    std::optional<std::string> callback_inspect_error;

    Harness* harness_ptr = nullptr;
    auto builder = TaskDefinitionBuilder("audit1166.failure", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            record_event("abort-enter");
            // Frozen probe shape: the abort handler inspects, and the sealed
            // Session must reject it. On the signalling thread the admission
            // check answers inline (ready rejection), so this neither blocks
            // nor deadlocks; a callback that *waited on another thread* for a
            // line-held read is the documented reentry boundary, probed
            // separately below.
            const auto inspection = consume(harness_ptr->inspect());
            if (inspection.has_value()) {
                record_event("inspect-settled");
            } else {
                record_event("inspect-rejected");
                callback_inspect_error = inspection.error().message;
            }
            signalled.store(true);
            record_event("abort-return");
        });
        record_event("phase");
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!release_phase.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        stop_observed_by_phase.store(runtime.stop_token().stop_requested());
        phase_done.store(true);
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));

    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    harness_ptr = harness.get();
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1166.failure"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_for(phase_ready, std::chrono::seconds(5)));

    record_event("fail-call");
    const auto outcome = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto entry = tx.append_entry(root.value(), "audit1166.fail", chord::Json(nullptr));
        if (!entry) return std::unexpected(entry.error());
        return {};
    }));
    const bool signalled_before_failure_return = signalled.load();
    record_event(outcome.has_value() ? "commit-settled" : "commit-rejected");

    CHECK(events == std::vector<std::string>({"phase",
            "fail-call",
            "storage-reject",
            "abort-enter",
            "inspect-rejected",
            "abort-return",
            "commit-rejected"}));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "injected 1166 failure");
    CHECK(signalled_before_failure_return);
    REQUIRE(callback_inspect_error.has_value());
    CHECK(*callback_inspect_error == "Session failed after a storage error; close and reopen it");

    // Session sealing: every later operation reports the frozen failure
    // message, never the original storage error (frozen `commit-rejected`).
    const auto later = consume(harness->commit([](Transaction& tx) -> support::ExpectedVoid {
        auto conversation = tx.create_conversation();
        if (!conversation) return std::unexpected(conversation.error());
        return {};
    }));
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error().message == "Session failed after a storage error; close and reopen it");

    // The signalled phase unwinds without close; draining close proves no
    // stale invocation identity survives the failure.
    release_phase.store(true);
    REQUIRE(wait_for(phase_done, std::chrono::seconds(5)));
    CHECK(stop_observed_by_phase.load());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("a task that completes normally leaves no running identity behind",
        "[durable][issue1166][lifecycle][spec]") {
    MemoryStorage memory;
    auto opened = consume(Harness::open(memory, HarnessOptions{.tasks = failure_task_definitions()}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    REQUIRE(harness->task_definition("audit1166.failure") != nullptr);
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1166.failure"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());

    // The phase commits its terminal state itself; settle is inline on this
    // thread for the memory backend.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<TaskRecord> record;
    while (std::chrono::steady_clock::now() < deadline) {
        auto current = memory.task(id);
        REQUIRE(current.has_value());
        if (current->has_value() && (*current)->state.status == "terminal") {
            record = **current;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(record.has_value());

    // No stale identity: a completed task is not derived as running, and the
    // terminal record is no longer offered as live work.
    const auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    for (const auto& row : inspection.value().tasks) {
        CHECK(row.record.id != id);
    }
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("aborting a task signals its active invocation and leaves no running identity behind",
        "[durable][issue1166][abort][spec]") {
    MemoryStorage memory;
    std::atomic_bool phase_ready{false};
    std::atomic_bool signalled{false};
    std::vector<std::string> events;
    auto builder = TaskDefinitionBuilder("audit1166.abort", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            events.push_back("abort-enter");
            signalled.store(true);
            events.push_back("abort-return");
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(memory, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    TaskId id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1166.abort"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_for(phase_ready, std::chrono::seconds(5)));

    const auto outcome = consume(harness->abort_task(id));
    REQUIRE(outcome.has_value());
    CHECK(signalled.load());
    CHECK(events == std::vector<std::string>({"abort-enter", "abort-return"}));

    // The abort handler settles the task terminal; until then the identity
    // may still be live, so poll the committed record with a bounded wait.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<TaskRecord> record;
    while (std::chrono::steady_clock::now() < deadline) {
        auto current = memory.task(id);
        REQUIRE(current.has_value());
        if (current->has_value() && (*current)->state.status == "terminal") {
            record = **current;
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(record.has_value());
    REQUIRE(record->state.outcome.has_value());
    CHECK(record->state.outcome->status == "aborted");

    // No stale identity after the abort settles: the task is not offered as
    // running.
    const auto inspection = consume(harness->inspect());
    REQUIRE(inspection.has_value());
    for (const auto& row : inspection.value().tasks) {
        CHECK(row.record.id != id);
    }
    REQUIRE(consume(harness->close()).has_value());
}

// Regression guard for the reviewed issue1165 candidate adopted by this
// change (worktree 1165, commit f9c268cfe): shutdown must release the
// scheduler mutex before running stop callbacks. A commit listener that calls
// back into `resume()` used to oppose the mutex held across `request_stop`,
// and the callback's bounded wait expired (RED: in-lock signalling; the
// candidate's copy-outside-then-signal makes the listener's `resume()`
// proceed, so no timeout occurs).
TEST_CASE("shutdown signals a stop callback without holding the scheduler mutex",
        "[durable][issue1166][lock][spec]") {
    MemoryStorage storage;
    std::atomic_bool phase_ready{false};
    std::atomic_bool stop_entered{false};
    std::atomic_bool listener_entered{false};
    std::atomic_bool resume_done{false};
    std::atomic_bool timed_out{false};
    auto builder = TaskDefinitionBuilder("audit1166.lock", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            stop_entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!resume_done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            timed_out.store(!resume_done.load());
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1166.lock"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_for(phase_ready, std::chrono::seconds(5)));
    REQUIRE(harness->session()
                    .subscribe_commits([&](const CommitPublication&) {
                        listener_entered.store(true);
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (!stop_entered.load() && std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::yield();
                        }
                        static_cast<void>(harness->resume());
                        resume_done.store(true);
                    })
                    .has_value());
    std::jthread holder([&] {
        static_cast<void>(consume(harness->commit([](Transaction& tx) -> support::ExpectedVoid {
            auto conversation = tx.create_conversation();
            if (!conversation) return std::unexpected(conversation.error());
            return {};
        })));
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!listener_entered.load() && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    const bool entered = listener_entered.load();
    const auto closed = consume(harness->close());
    // The gates are released before joining: the holder commit completes once
    // close's stop signal lets the listener's `resume()` proceed.
    holder.join();
    REQUIRE(entered);
    CHECK(stop_entered.load());
    CHECK_FALSE(timed_out.load());
    CHECK(resume_done.load());
    REQUIRE(closed.has_value());
}

// Frozen close authority: close signals every invocation and waits for it
// before the backend closes (frozen pi `beforeClose -> TaskScheduler.join`).
// A close that returns while a running stop callback is still executing, or
// that never delivers the signal, fails this case.
TEST_CASE("closing the harness signals an active invocation and waits for its stop callback",
        "[durable][issue1166][close][spec]") {
    MemoryStorage storage;
    std::atomic_bool phase_ready{false};
    std::atomic_bool callback_entered{false};
    std::atomic_bool release_callback{false};
    std::atomic_bool callback_finished{false};
    auto builder = TaskDefinitionBuilder("audit1166.close", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            callback_entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!release_callback.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            callback_finished.store(true);
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1166.close"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_for(phase_ready, std::chrono::seconds(5)));

    std::atomic_bool close_success{false};
    std::jthread closer([&] {
        close_success.store(consume(harness->close()).has_value());
    });
    REQUIRE(wait_for(callback_entered, std::chrono::seconds(5)));
    // Close must not settle while the stop callback still runs.
    CHECK_FALSE(callback_finished.load());
    release_callback.store(true);
    closer.join();
    CHECK(callback_finished.load());
    CHECK(close_success.load());
}

// Defect demonstration, pinned as the current boundary (adapted from the
// issue1165 WIP probe): a stop callback that waits for a committed Session
// read cannot be served while the signal runs on the publication line — the
// line admits the read only after the signalling settlement completes, and
// that completion waits for the callback. The 500 ms expiry is structural,
// not a timing race: `read_completed` cannot become true before the callback
// returns. Deferring stop signals off the Session line is a separate design
// change and must update this demonstration and the documented restriction.
TEST_CASE("a stop callback that waits for a committed read is served only after its publication line drains",
        "[durable][issue1166][reentry]") {
    MemoryStorage storage;
    std::atomic_bool phase_ready{false};
    std::atomic_bool callback_entered{false};
    std::atomic_bool read_completed{false};
    std::atomic_bool callback_timed_out{false};
    Harness* harness_ptr = nullptr;
    auto builder = TaskDefinitionBuilder("audit1166.reentry", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            callback_entered.store(true);
            std::move(harness_ptr->session().read_committed([](Storage&) -> support::Expected<chord::Json> {
                return chord::Json(nullptr);
            })).start([&](support::Expected<chord::Json>) noexcept { read_completed.store(true); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!read_completed.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            callback_timed_out.store(!read_completed.load());
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    });
    builder.abort([](const TaskRecord&, TaskRuntime& runtime) {
        return runtime.commit([](Transaction&, const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
            return NextTaskState::terminal(TaskOutcome{.status = "aborted"});
        });
    });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> definitions;
    definitions.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(definitions)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    harness_ptr = harness.get();
    TaskId task_id{};
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1166.reentry"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        task_id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    REQUIRE(wait_for(phase_ready, std::chrono::seconds(5)));

    const auto outcome = consume(harness->abort_task(task_id));
    CHECK(outcome.has_value());
    CHECK(callback_entered.load());
    CHECK(callback_timed_out.load());
    // Once the line drains, the queued read runs to completion.
    CHECK(read_completed.load());
    REQUIRE(consume(harness->close()).has_value());
}
