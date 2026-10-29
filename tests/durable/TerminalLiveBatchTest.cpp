// Frozen-evidence-driven tests for issue #1168: the public durable Storage
// task-write contract for terminal↔live batches, verified against the
// `audit-1168` family captured from frozen pi v1.3.0
// (71023aec1546d6d677a65669c5665c0927ce0dd0). The frozen Session transaction
// rejects replacing a terminal task (frozen pi `session/transaction.ts`
// `#assemble`); the public Storage accepts the same batch with the last write
// winning, on every backend. Pike keeps the invariant at the Session layer
// and matches the frozen Storage contract.

#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/SqliteStorage.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests;
using namespace cch::tests::durable;

namespace {

constexpr std::string_view kTerminalLiveArtifact = "fixtures/pi-v1.3.0/audit-1168/terminal-live.json";
constexpr std::string_view kInterchangeArtifact = "fixtures/pi-v1.3.0/audit-1168/interchange.json";

/// One frozen expectation, loaded once per case.
[[nodiscard]] FrozenScenario frozen(std::string_view artifact, std::string_view name) {
    auto scenario = FrozenScenario::load(artifact, name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The terminal task record of the frozen matrix: task 2 in conversation 1.
[[nodiscard]] TaskRecord terminal_task() {
    TaskRecord record{};
    record.id = TaskId{.value = 2};
    record.conversation_id = ConversationId{.value = 1};
    record.kind = "work";
    record.version = 1;
    record.state.status = "terminal";
    record.state.outcome = TaskOutcome{.status = "completed"};
    return record;
}

/// The live task record of the frozen matrix with one checkpoint phase.
[[nodiscard]] TaskRecord live_task(std::string_view phase) {
    TaskRecord record = terminal_task();
    record.state.status = "pending";
    record.state.outcome = std::nullopt;
    record.state.checkpoint = chord::Json{chord::Json::Object{{"phase", std::string{phase}}}};
    return record;
}

[[nodiscard]] StorageWrite task_write(const TaskRecord& record) {
    StorageWrite write{};
    write.type = "task";
    write.task = record;
    return write;
}

/// One invalid write: a document change for a document that does not exist.
[[nodiscard]] StorageWrite invalid_write() {
    StorageWrite write{};
    write.type = "document.change";
    write.document_id = DocumentId{.value = 99};
    write.document_version = 1;
    write.document_value = chord::Json{chord::Json::Object{}};
    return write;
}

[[nodiscard]] bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const bool found = std::filesystem::exists(path, error);
    REQUIRE_FALSE(error);
    return found;
}

/// Assert one storage read resolves to the live task 2 with the phase.
void check_live_task(const support::Expected<std::optional<TaskRecord>>& result, std::string_view phase) {
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK((*result)->state.status == "pending");
    REQUIRE((*result)->state.checkpoint.has_value());
    CHECK(*(*result)->state.checkpoint == chord::Json{chord::Json::Object{{"phase", std::string{phase}}}});
}

/// The frozen `state.status` of a frozen task record member (`immediate` or `reopen`).
[[nodiscard]] std::string frozen_status(const FrozenScenario& scenario, std::string_view member) {
    const auto* record = scenario.object_member(member);
    REQUIRE(record != nullptr);
    const auto* state = support::json_member(*record, "state");
    REQUIRE(state != nullptr);
    const auto* object = state->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto* status = support::json_member(*object, "status");
    REQUIRE(status != nullptr);
    return status->get<std::string>();
}

/// The frozen `state.checkpoint` of a frozen task record member.
[[nodiscard]] const support::JsonValue* frozen_checkpoint(const FrozenScenario& scenario, std::string_view member) {
    const auto* record = scenario.object_member(member);
    REQUIRE(record != nullptr);
    const auto* state = support::json_member(*record, "state");
    REQUIRE(state != nullptr);
    const auto* object = state->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    return support::json_member(*object, "checkpoint");
}

/// Assert one storage read resolves to the frozen reopened record.
void check_frozen_reopen_task(
        const support::Expected<std::optional<TaskRecord>>& result, const FrozenScenario& scenario) {
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK((*result)->state.status == frozen_status(scenario, "reopen"));
    const auto* checkpoint = frozen_checkpoint(scenario, "reopen");
    REQUIRE(checkpoint != nullptr);
    REQUIRE((*result)->state.checkpoint.has_value());
    CHECK(json_equals(*(*result)->state.checkpoint, *checkpoint));
}

/// Materialize the embedded frozen JSONL files of one interchange fixture scenario.
void materialize_fixture(const FrozenScenario& scenario, TempWorkspace& workspace) {
    const auto* files = scenario.member("files");
    REQUIRE(files != nullptr);
    const auto* list = files->get_if<support::JsonValue::array_t>();
    REQUIRE(list != nullptr);
    for (const auto& value : *list) {
        const auto* file = value.get_if<support::JsonValue::object_t>();
        REQUIRE(file != nullptr);
        const auto name = support::json_string_member(*file, "name");
        const auto contents = support::json_string_member(*file, "text");
        REQUIRE(name);
        REQUIRE(contents);
        workspace.write(std::string{*name}, std::string{*contents});
    }
}

} // namespace

TEST_CASE("public memory storage settles a new task terminal then live in one batch with the last write",
        "[durable][storage][issue1168][spec]") {
    const auto expected = frozen(kTerminalLiveArtifact,
            "memory storage settles a new task terminal then live in one batch with the last write");
    MemoryStorage storage;
    const std::vector<StorageWrite> writes{task_write(terminal_task()), task_write(live_task("again"))};
    const Outcome<Seq> result = consume(storage.commit(writes));
    REQUIRE(result.has_value());
    REQUIRE(expected.number_member("seq").has_value());
    CHECK(result.value() == static_cast<Seq>(*expected.number_member("seq")));
    check_live_task(storage.task(TaskId{.value = 2}), "again");
    // The same instance continues the sequence.
    const std::vector<StorageWrite> next{task_write(live_task("next"))};
    const Outcome<Seq> continued = consume(storage.commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
}

TEST_CASE("public memory storage settles a new task live then terminal in one batch with the last write",
        "[durable][storage][issue1168][spec]") {
    MemoryStorage storage;
    const std::vector<StorageWrite> writes{task_write(live_task("first")), task_write(terminal_task())};
    const Outcome<Seq> result = consume(storage.commit(writes));
    REQUIRE(result.has_value());
    CHECK(result.value() == 1);
    const auto task = storage.task(TaskId{.value = 2});
    REQUIRE(task);
    REQUIRE(task->has_value());
    CHECK((*task)->state.status == "terminal");
}

TEST_CASE("public memory storage accepts terminal then live for an already-live task",
        "[durable][storage][issue1168][spec]") {
    MemoryStorage storage;
    const std::vector<StorageWrite> first{task_write(live_task("original"))};
    REQUIRE(consume(storage.commit(first)).has_value());
    const std::vector<StorageWrite> second{task_write(terminal_task()), task_write(live_task("again"))};
    const Outcome<Seq> result = consume(storage.commit(second));
    REQUIRE(result.has_value());
    CHECK(result.value() == 2);
    check_live_task(storage.task(TaskId{.value = 2}), "again");
}

TEST_CASE("public memory storage revives an existing terminal task with one live write",
        "[durable][storage][issue1168][spec]") {
    const auto expected =
            frozen(kTerminalLiveArtifact, "memory storage revives an existing terminal task with one live write");
    MemoryStorage storage;
    const std::vector<StorageWrite> terminal{task_write(terminal_task())};
    REQUIRE(consume(storage.commit(terminal)).has_value());
    const std::vector<StorageWrite> revive{task_write(live_task("revived"))};
    const Outcome<Seq> result = consume(storage.commit(revive));
    REQUIRE(result.has_value());
    REQUIRE(expected.number_member("reviveSeq").has_value());
    CHECK(result.value() == static_cast<Seq>(*expected.number_member("reviveSeq")));
    check_live_task(storage.task(TaskId{.value = 2}), "revived");
}

TEST_CASE("public memory storage rejects a mixed invalid batch atomically and consumes no sequence",
        "[durable][storage][issue1168][spec]") {
    const auto expected = frozen(
            kTerminalLiveArtifact, "memory storage rejects a mixed invalid batch atomically and consumes no sequence");
    MemoryStorage storage;
    const std::vector<StorageWrite> writes{task_write(terminal_task()), invalid_write()};
    const Outcome<Seq> result = consume(storage.commit(writes));
    REQUIRE_FALSE(result.has_value());
    CHECK(expected.member("rejected")->get<bool>());
    const auto task = storage.task(TaskId{.value = 2});
    REQUIRE(task);
    CHECK_FALSE(task->has_value());
    const std::vector<StorageWrite> empty;
    const Outcome<Seq> next = consume(storage.commit(empty));
    REQUIRE(next.has_value());
    REQUIRE(expected.number_member("nextEmptyCommitSeq").has_value());
    CHECK(next.value() == static_cast<Seq>(*expected.number_member("nextEmptyCommitSeq")));
}

TEST_CASE("public jsonl storage settles a new task terminal then live in one batch with the last write",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(kTerminalLiveArtifact,
            "jsonl storage settles a new task terminal then live in one batch with the last write");
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> writes{task_write(terminal_task()), task_write(live_task("again"))};
    const Outcome<Seq> result = consume(storage->commit(writes));
    REQUIRE(result.has_value());
    REQUIRE(expected.number_member("seq").has_value());
    CHECK(result.value() == static_cast<Seq>(*expected.number_member("seq")));
    check_live_task(storage->task(TaskId{.value = 2}), "again");
    // The final write is live, so the sidecar is retained.
    CHECK(path_exists(workspace.path() / "task-2.jsonl"));
    REQUIRE(storage->close().has_value());
    // The reopen replays the terminal record and then the live sidecar.
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    std::unique_ptr<JsonlStorage> reader = std::move(*reopened);
    check_live_task(reader->task(TaskId{.value = 2}), "again");
    const std::vector<StorageWrite> next{task_write(live_task("next"))};
    const Outcome<Seq> continued = consume(reader->commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
}

TEST_CASE("public jsonl storage settles a new task live then terminal in one batch and reclaims the sidecar",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(kTerminalLiveArtifact,
            "jsonl storage settles a new task live then terminal in one batch with the last write");
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> writes{task_write(live_task("first")), task_write(terminal_task())};
    const Outcome<Seq> result = consume(storage->commit(writes));
    REQUIRE(result.has_value());
    CHECK(result.value() == 1);
    const auto task = storage->task(TaskId{.value = 2});
    REQUIRE(task);
    REQUIRE(task->has_value());
    CHECK((*task)->state.status == "terminal");
    // The terminal publication reclaims the same-batch live sidecar.
    REQUIRE(storage->close().has_value());
    CHECK_FALSE(path_exists(workspace.path() / "task-2.jsonl"));
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    std::unique_ptr<JsonlStorage> reader = std::move(*reopened);
    const auto after = reader->task(TaskId{.value = 2});
    REQUIRE(after);
    REQUIRE(after->has_value());
    CHECK((*after)->state.status == "terminal");
    // The reclaimed store continues at the frozen sequence.
    const std::vector<StorageWrite> next{task_write(live_task("after-terminal"))};
    const Outcome<Seq> continued = consume(reader->commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
}

TEST_CASE("public jsonl storage accepts terminal then live for an already-live task",
        "[durable][jsonl][storage][issue1168][spec]") {
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> first{task_write(live_task("original"))};
    REQUIRE(consume(storage->commit(first)).has_value());
    const std::vector<StorageWrite> second{task_write(terminal_task()), task_write(live_task("again"))};
    const Outcome<Seq> result = consume(storage->commit(second));
    REQUIRE(result.has_value());
    CHECK(result.value() == 2);
    REQUIRE(storage->close().has_value());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    std::unique_ptr<JsonlStorage> reader = std::move(*reopened);
    check_live_task(reader->task(TaskId{.value = 2}), "again");
}

TEST_CASE("public jsonl storage revives an existing terminal task with one live write",
        "[durable][jsonl][storage][issue1168][spec]") {
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> terminal{task_write(terminal_task())};
    REQUIRE(consume(storage->commit(terminal)).has_value());
    // The terminal publication reclaims the task sidecar entirely.
    REQUIRE(storage->close().has_value());
    CHECK_FALSE(path_exists(workspace.path() / "task-2.jsonl"));
    auto revived_open = JsonlStorage::open(workspace.path());
    REQUIRE(revived_open);
    std::unique_ptr<JsonlStorage> live = std::move(*revived_open);
    const std::vector<StorageWrite> revive{task_write(live_task("revived"))};
    const Outcome<Seq> result = consume(live->commit(revive));
    REQUIRE(result.has_value());
    CHECK(result.value() == 2);
    check_live_task(live->task(TaskId{.value = 2}), "revived");
    REQUIRE(live->close().has_value());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    std::unique_ptr<JsonlStorage> reader = std::move(*reopened);
    check_live_task(reader->task(TaskId{.value = 2}), "revived");
}

TEST_CASE("public jsonl storage rejects a mixed invalid batch atomically and consumes no sequence",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(
            kTerminalLiveArtifact, "jsonl storage rejects a mixed invalid batch atomically and consumes no sequence");
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> writes{task_write(terminal_task()), invalid_write()};
    const Outcome<Seq> result = consume(storage->commit(writes));
    REQUIRE_FALSE(result.has_value());
    CHECK(expected.member("rejected")->get<bool>());
    // Validation precedes publication: no marker and no sidecar exist at all.
    CHECK_FALSE(path_exists(workspace.path() / "main.jsonl"));
    CHECK_FALSE(path_exists(workspace.path() / "task-2.jsonl"));
    const std::vector<StorageWrite> empty;
    const Outcome<Seq> next = consume(storage->commit(empty));
    REQUIRE(next.has_value());
    REQUIRE(expected.number_member("nextEmptyCommitSeq").has_value());
    CHECK(next.value() == static_cast<Seq>(*expected.number_member("nextEmptyCommitSeq")));
    REQUIRE(storage->close().has_value());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
}

TEST_CASE("public sqlite storage settles a new task terminal then live in one batch with the last write",
        "[durable][sqlite][storage][issue1168][spec]") {
    const auto expected = frozen(kTerminalLiveArtifact,
            "sqlite storage settles a new task terminal then live in one batch with the last write");
    TempWorkspace workspace;
    auto opened = SqliteStorage::open(workspace.path() / "store.sqlite");
    REQUIRE(opened);
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> writes{task_write(terminal_task()), task_write(live_task("again"))};
    const Outcome<Seq> result = consume(storage->commit(writes));
    REQUIRE(result.has_value());
    REQUIRE(expected.number_member("seq").has_value());
    CHECK(result.value() == static_cast<Seq>(*expected.number_member("seq")));
    check_live_task(storage->task(TaskId{.value = 2}), "again");
    REQUIRE(storage->close().has_value());
    auto reopened = SqliteStorage::open(workspace.path() / "store.sqlite");
    REQUIRE(reopened);
    std::unique_ptr<SqliteStorage> reader = std::move(*reopened);
    check_live_task(reader->task(TaskId{.value = 2}), "again");
    const std::vector<StorageWrite> next{task_write(live_task("next"))};
    const Outcome<Seq> continued = consume(reader->commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
}

TEST_CASE("public sqlite storage revives an existing terminal task with one live write",
        "[durable][sqlite][storage][issue1168][spec]") {
    TempWorkspace workspace;
    auto opened = SqliteStorage::open(workspace.path() / "store.sqlite");
    REQUIRE(opened);
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> terminal{task_write(terminal_task())};
    REQUIRE(consume(storage->commit(terminal)).has_value());
    const std::vector<StorageWrite> revive{task_write(live_task("revived"))};
    const Outcome<Seq> result = consume(storage->commit(revive));
    REQUIRE(result.has_value());
    CHECK(result.value() == 2);
    REQUIRE(storage->close().has_value());
    auto reopened = SqliteStorage::open(workspace.path() / "store.sqlite");
    REQUIRE(reopened);
    std::unique_ptr<SqliteStorage> reader = std::move(*reopened);
    check_live_task(reader->task(TaskId{.value = 2}), "revived");
}

TEST_CASE("public sqlite storage rejects a mixed invalid batch atomically and consumes no sequence",
        "[durable][sqlite][storage][issue1168][spec]") {
    const auto expected = frozen(
            kTerminalLiveArtifact, "sqlite storage rejects a mixed invalid batch atomically and consumes no sequence");
    TempWorkspace workspace;
    auto opened = SqliteStorage::open(workspace.path() / "store.sqlite");
    REQUIRE(opened);
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    const std::vector<StorageWrite> writes{task_write(terminal_task()), invalid_write()};
    const Outcome<Seq> result = consume(storage->commit(writes));
    REQUIRE_FALSE(result.has_value());
    CHECK(expected.member("rejected")->get<bool>());
    const auto task = storage->task(TaskId{.value = 2});
    REQUIRE(task);
    CHECK_FALSE(task->has_value());
    const std::vector<StorageWrite> empty;
    const Outcome<Seq> next = consume(storage->commit(empty));
    REQUIRE(next.has_value());
    REQUIRE(expected.number_member("nextEmptyCommitSeq").has_value());
    CHECK(next.value() == static_cast<Seq>(*expected.number_member("nextEmptyCommitSeq")));
}

TEST_CASE("a frozen pi jsonl store with a terminal then live task batch reopens with the pending task",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(kInterchangeArtifact,
            "frozen pi jsonl store with a terminal then live task batch reopens with the pending task");
    TempWorkspace workspace;
    materialize_fixture(expected, workspace);
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    check_frozen_reopen_task(storage->task(TaskId{.value = 2}), expected);
    // The store continues at the frozen sequence.
    const std::vector<StorageWrite> next{task_write(live_task("continued"))};
    const Outcome<Seq> continued = consume(storage->commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
    REQUIRE(storage->close().has_value());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    std::unique_ptr<JsonlStorage> reader = std::move(*reopened);
    check_live_task(reader->task(TaskId{.value = 2}), "continued");
}

TEST_CASE("a frozen pi jsonl store reopen rejects a mismatched live sidecar",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(kInterchangeArtifact,
            "frozen pi jsonl store with a terminal then live task batch reopens with the pending task");
    TempWorkspace workspace;
    materialize_fixture(expected, workspace);
    // A live sidecar whose payload id disagrees with the marker is corruption:
    // the reopen must fail closed instead of silently reviving the task.
    auto sidecar = workspace.read("task-2.jsonl");
    const auto position = sidecar.find("\"id\":2");
    REQUIRE(position != std::string::npos);
    sidecar.replace(position, std::string{"\"id\":2"}.size(), "\"id\":9");
    workspace.write("task-2.jsonl", sidecar);
    auto rejected = JsonlStorage::open(workspace.path());
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "Confirmed task sidecar data does not match commit 1");
}

TEST_CASE("a frozen pi jsonl store with a live then terminal task batch reopens with the terminal task and a reclaimed "
          "sidecar",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(kInterchangeArtifact,
            "frozen pi jsonl store with a live then terminal task batch reopens with the terminal task and a reclaimed "
            "sidecar");
    TempWorkspace workspace;
    materialize_fixture(expected, workspace);
    // The frozen store carries only the main marker: the same-batch live
    // sidecar reference is optional because the final write is terminal.
    CHECK_FALSE(path_exists(workspace.path() / "task-2.jsonl"));
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    const auto task = storage->task(TaskId{.value = 2});
    REQUIRE(task);
    REQUIRE(task->has_value());
    CHECK((*task)->state.status == frozen_status(expected, "reopen"));
    const std::vector<StorageWrite> next{task_write(live_task("after-terminal"))};
    const Outcome<Seq> continued = consume(storage->commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
}

TEST_CASE("a frozen pi jsonl store with a revived terminal task reopens with the pending task",
        "[durable][jsonl][storage][issue1168][spec]") {
    const auto expected = frozen(
            kInterchangeArtifact, "frozen pi jsonl store with a revived terminal task reopens with the pending task");
    TempWorkspace workspace;
    materialize_fixture(expected, workspace);
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened);
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    check_frozen_reopen_task(storage->task(TaskId{.value = 2}), expected);
    const std::vector<StorageWrite> next{task_write(live_task("continued"))};
    const Outcome<Seq> continued = consume(storage->commit(next));
    REQUIRE(continued.has_value());
    REQUIRE(expected.number_member("nextCommitSeq").has_value());
    CHECK(continued.value() == static_cast<Seq>(*expected.number_member("nextCommitSeq")));
}
