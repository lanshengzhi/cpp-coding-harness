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
class FailureStorage final : public Storage {
public:
    explicit FailureStorage(Storage& inner) : inner_(inner) {}

    [[nodiscard]] support::AsyncResult<Seq> commit(std::span<const StorageWrite> writes) override {
        for (const auto& write : writes) {
            if (write.entry && write.entry->kind == "audit1165.fail")
                return support::AsyncResult<Seq>{
                        std::unexpected(support::make_error(support::ErrorCode::Process, "injected 1165 failure"))};
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
    std::jthread holder([&] {
        static_cast<void>(consume(harness->session().read_committed([&](Storage&) -> support::Expected<chord::Json> {
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
    std::jthread probe([&] {
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
    REQUIRE(did_enter);
    CHECK_FALSE(inconsistent.load());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("shutdown signals stop without holding the scheduler mutex", "[durable][issue1165-lock]") {
    MemoryStorage storage;
    std::atomic_bool phase_ready{false};
    std::atomic_bool stop_entered{false};
    std::atomic_bool holder_entered{false};
    std::atomic_bool resume_done{false};
    std::atomic_bool timed_out{false};
    auto builder = TaskDefinitionBuilder("audit1165.lock", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            stop_entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!resume_done.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            timed_out.store(!resume_done.load());
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline)
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
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.lock"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    const auto phase_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!phase_ready.load() && std::chrono::steady_clock::now() < phase_deadline)
        std::this_thread::yield();
    REQUIRE(phase_ready.load());
    REQUIRE(harness->session()
                    .subscribe_commits([&](const CommitPublication&) {
                        holder_entered.store(true);
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (!stop_entered.load() && std::chrono::steady_clock::now() < deadline)
                            std::this_thread::yield();
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
    while (!holder_entered.load() && std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    const bool entered = holder_entered.load();
    auto closed = consume(harness->close());
    holder.join();
    REQUIRE(entered);
    CHECK(stop_entered.load());
    CHECK_FALSE(timed_out.load());
    CHECK(resume_done.load());
    CHECK(closed.has_value());
}

TEST_CASE("stop callback waiting for a reentrant committed read blocks its publication line",
        "[durable][issue1165-reentry]") {
    MemoryStorage storage;
    std::atomic_bool phase_ready{false};
    std::atomic_bool callback_entered{false};
    std::atomic_bool read_completed{false};
    std::atomic_bool callback_timed_out{false};
    auto builder = TaskDefinitionBuilder("audit1088.reentry", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    Harness* harness_ptr = nullptr;
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            callback_entered.store(true);
            std::move(harness_ptr->session().read_committed([](Storage&) -> support::Expected<chord::Json> {
                return chord::Json(nullptr);
            })).start([&](support::Expected<chord::Json>) noexcept { read_completed.store(true); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!read_completed.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            callback_timed_out.store(!read_completed.load());
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
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
        auto task = tx.create_task(*harness->task_definition("audit1088.reentry"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        task_id = *task;
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!phase_ready.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(phase_ready.load());
    const auto outcome = consume(harness->abort_task(task_id));
    CHECK(outcome.has_value());
    CHECK(callback_entered.load());
    CHECK(callback_timed_out.load());
    CHECK(read_completed.load());
    REQUIRE(consume(harness->close()).has_value());
}

TEST_CASE("harness close waits for an active stop callback", "[durable][issue1165-close]") {
    MemoryStorage storage;
    std::atomic_bool phase_ready{false};
    std::atomic_bool callback_entered{false};
    std::atomic_bool release_callback{false};
    std::atomic_bool close_done{false};
    std::atomic_bool callback_finished{false};
    auto builder = TaskDefinitionBuilder("audit1165.close", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] {
            callback_entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!release_callback.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            callback_finished.store(true);
        });
        phase_ready.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!runtime.stop_token().stop_requested() && std::chrono::steady_clock::now() < deadline)
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
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.close"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    const auto phase_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!phase_ready.load() && std::chrono::steady_clock::now() < phase_deadline)
        std::this_thread::yield();
    REQUIRE(phase_ready.load());
    std::atomic_bool close_success{false};
    std::jthread closer([&] {
        close_success.store(consume(harness->close()).has_value());
        close_done.store(true);
    });
    const auto callback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!callback_entered.load() && std::chrono::steady_clock::now() < callback_deadline)
        std::this_thread::yield();
    const bool entered = callback_entered.load();
    const bool early_close = close_done.load();
    release_callback.store(true);
    closer.join();
    REQUIRE(entered);
    CHECK_FALSE(early_close);
    CHECK(callback_finished.load());
    CHECK(close_done.load());
    CHECK(close_success.load());
}

TEST_CASE("storage failure signals an active invocation", "[durable][issue1165-failure]") {
    MemoryStorage memory;
    FailureStorage storage(memory);
    std::atomic_bool phase_ready{false};
    std::atomic_bool callback_entered{false};
    std::atomic_bool release_phase{false};
    auto builder = TaskDefinitionBuilder("audit1165.failure", 1);
    builder.initial([](const chord::Json&) { return chord::Json(chord::Json::Object{{"phase", "run"}}); });
    builder.phase("run", [&](const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
        std::stop_callback callback(runtime.stop_token(), [&] { callback_entered.store(true); });
        phase_ready.store(true);
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
    const auto root = consume(harness->root());
    REQUIRE(root.has_value());
    REQUIRE(consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto task = tx.create_task(*harness->task_definition("audit1165.failure"), chord::Json(nullptr));
        if (!task) return std::unexpected(task.error());
        return {};
    })).has_value());
    REQUIRE(harness->resume().has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!phase_ready.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool ready = phase_ready.load();
    auto outcome = consume(harness->commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto entry = tx.append_entry(root.value(), "audit1165.fail", chord::Json(nullptr));
        if (!entry) return std::unexpected(entry.error());
        return {};
    }));
    const bool signalled_before_failure_return = callback_entered.load();
    release_phase.store(true);
    REQUIRE(ready);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "injected 1165 failure");
    CHECK(signalled_before_failure_return);
    static_cast<void>(consume(harness->close()));
}
