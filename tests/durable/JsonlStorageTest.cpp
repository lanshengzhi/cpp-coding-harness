#include <cch/durable/Harness.hpp>
#include <cch/durable/JsonlStorage.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/JsonlIoTestHooks.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <mutex>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests;
using namespace cch::tests::durable;

namespace {

/// The frozen JSONL artifact of the `audit-1054` family.
constexpr std::string_view kJsonlArtifact = "fixtures/pi-v1.3.0/audit-1054/jsonl-store.json";

constexpr std::string_view kOrderScenario = "a JSONL commit appends every sidecar before the main marker";
constexpr std::string_view kRecoveryScenario =
        "JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail";
constexpr std::string_view kMissingScenario = "JSONL refuses a main marker whose confirmed sidecar record is missing";
constexpr std::string_view kPoisonScenario =
        "a JSONL main-marker I/O failure poisons the instance while reopen recovers the published prefix";
constexpr std::string_view kCheckpointScenario =
        "a JSONL checkpoint replaces a current-only sidecar and retirement removes it";
constexpr std::string_view kFsyncScenario = "JSONL fsync flushes sidecar bytes before the main marker is appended";

/// The document definition of the captured scenarios: `{text, revision}`.
[[nodiscard]] DocumentDefinition latest_document() {
    return define_document(std::string{"audit1054.latest"}, std::move_only_function<chord::Json()>{[] {
        return chord::Json{chord::Json::Object{{"text", chord::Json{std::string{""}}}, {"revision", chord::Json{0}}}};
    }});
}

[[nodiscard]] chord::Path member_path(std::string key) { return chord::Path{chord::path_key(std::move(key))}; }

[[nodiscard]] Outcome<void> run_commit(
        Session& session, std::move_only_function<support::Expected<void>(Transaction&)> change) {
    return consume(session.commit(std::move(change)));
}

[[nodiscard]] ConversationId create_conversation(Session& session) {
    std::optional<ConversationId> created;
    auto outcome = run_commit(session, [&created](Transaction& transaction) -> support::Expected<void> {
        auto record = transaction.create_conversation();
        if (!record) {
            return std::unexpected(std::move(record.error()));
        }
        created = record->id;
        return {};
    });
    if (!outcome.has_value()) {
        FAIL(outcome.error().message);
    }
    REQUIRE(outcome.has_value());
    REQUIRE(created.has_value());
    return *created;
}

[[nodiscard]] std::vector<support::JsonValue> json_lines(std::string_view text) {
    std::vector<support::JsonValue> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        REQUIRE(end != std::string_view::npos);
        auto parsed = support::read_json(text.substr(start, end - start));
        REQUIRE(parsed.has_value());
        lines.push_back(std::move(*parsed));
        start = end + 1;
    }
    return lines;
}

[[nodiscard]] std::string json_text(const support::JsonValue& value) {
    auto text = support::write_json(value);
    REQUIRE(text.has_value());
    return *text;
}

/// Canonical comparison text for the frozen `audit-1054` records. The frozen
/// artifacts are compared by value, not by their member order, so the
/// comparison normalizes explicitly instead of relying on a container order
/// (#1149).
[[nodiscard]] std::string canonical_text(const support::JsonValue& value) {
    auto text = support::canonical_json(value);
    REQUIRE(text.has_value());
    return *text;
}

[[nodiscard]] support::JsonValue json_object(
        std::initializer_list<std::pair<std::string, support::JsonValue>> members) {
    support::JsonValue::object_t object;
    for (const auto& [key, value] : members) {
        object.emplace(key, value);
    }
    return support::JsonValue{std::move(object)};
}

void require_files_match(const TempWorkspace& workspace, const support::JsonValue::array_t& expected_files) {
    std::vector<std::string> actual_names;
    for (const auto& entry : std::filesystem::directory_iterator(workspace.path())) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".jsonl")) {
            actual_names.push_back(name);
        }
    }
    std::vector<std::string> expected_names;
    for (const support::JsonValue& file_value : expected_files) {
        const auto* file = file_value.get_if<support::JsonValue::object_t>();
        REQUIRE(file != nullptr);
        const auto name = support::json_string_member(*file, "name");
        const auto* lines = support::json_array_member(*file, "lines");
        REQUIRE(name.has_value());
        REQUIRE(lines != nullptr);
        expected_names.emplace_back(*name);

        const auto actual_lines = json_lines(workspace.read(std::string{*name}));
        REQUIRE(actual_lines.size() == lines->size());
        for (std::size_t index = 0; index < lines->size(); ++index) {
            CHECK(canonical_text(actual_lines[index]) == canonical_text((*lines)[index]));
        }
    }
    std::sort(expected_names.begin(), expected_names.end());
    std::sort(actual_names.begin(), actual_names.end());
    CHECK(actual_names == expected_names);
}

void append_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    stream << text;
    REQUIRE(stream.good());
}

/// The physical JSONL operations one store performed, recorded through the
/// private I/O seam. The shape mirrors the frozen `InstrumentedEnv` trace of
/// `packages/durable/test/jsonl-storage.test.ts`, so a C++ trace can be
/// compared with the frozen operation list directly.
struct JsonlIoLog {
    std::vector<std::string> operations;
    std::map<testing::JsonlIoOperation, std::size_t> calls;
    std::optional<testing::JsonlIoOperation> failing_operation;
    std::size_t failing_call{0};
};

JsonlIoLog& jsonl_io_log() {
    static JsonlIoLog log;
    return log;
}

[[nodiscard]] std::string describe_io(const testing::JsonlIoEvent& event) {
    switch (event.operation) {
    case testing::JsonlIoOperation::Append:
        return "append:" + event.file;
    case testing::JsonlIoOperation::Flush:
        return "flush:" + event.file;
    case testing::JsonlIoOperation::Write:
        return "write:" + event.file;
    case testing::JsonlIoOperation::Rename:
        return "rename:" + event.file + "->" + event.destination;
    case testing::JsonlIoOperation::Remove:
        return "remove:" + event.file;
    }
    return {};
}

/// The capture hook. It holds no test-case state, so an assertion that
/// abandons a test frame leaves no dangling callback behind.
bool record_jsonl_io(const testing::JsonlIoEvent& event) {
    JsonlIoLog& log = jsonl_io_log();
    log.operations.push_back(describe_io(event));
    const std::size_t call = ++log.calls[event.operation];
    if (!log.failing_operation.has_value() || *log.failing_operation != event.operation || log.failing_call != call) {
        return false;
    }
    log.failing_operation.reset();
    return true;
}

/// Install the recorder and forget every earlier operation.
void start_io_trace() {
    jsonl_io_log() = JsonlIoLog{};
    testing::set_jsonl_io_hook(&record_jsonl_io);
}

/// Forget the recorded operations but keep the recorder installed.
void clear_io_trace() {
    jsonl_io_log().operations.clear();
    jsonl_io_log().calls.clear();
}

/// Fail the nth call of one operation (1-based), like the frozen
/// `InstrumentedEnv.fail`.
void fail_io_call(testing::JsonlIoOperation operation, std::size_t call) {
    jsonl_io_log().failing_operation = operation;
    jsonl_io_log().failing_call = call;
}

void stop_io_trace() {
    testing::clear_jsonl_io_hook();
    jsonl_io_log() = JsonlIoLog{};
}

[[nodiscard]] std::vector<std::string> frozen_strings(
        const support::JsonValue::object_t& object, std::string_view name) {
    const auto* values = support::json_array_member(object, name);
    REQUIRE(values != nullptr);
    std::vector<std::string> result;
    result.reserve(values->size());
    for (const support::JsonValue& value : *values) {
        const auto* text = value.get_if<std::string>();
        REQUIRE(text != nullptr);
        result.push_back(*text);
    }
    return result;
}

} // namespace

// The registered pi fixture includes task 6 in a sidecar with a pending
// checkpoint. This first slice tests public recovery; the fixture itself has
// no executable Harness definition or submission to establish continuation.
TEST_CASE("a pi JSONL pending task is recovered from its sidecar", "[durable][jsonl][issue1085]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1054/interchange.json",
            "jsonl store created by pi reopens with unfinished task, documents and checkpoints and continues");
    REQUIRE(frozen);
    const auto* fixture = frozen->object_member("fixture");
    REQUIRE(fixture != nullptr);
    const auto* files = support::json_array_member(*fixture, "files");
    REQUIRE(files != nullptr);
    TempWorkspace workspace;
    for (const auto& value : *files) {
        const auto* file = value.get_if<support::JsonValue::object_t>();
        REQUIRE(file != nullptr);
        const auto name = support::json_string_member(*file, "name");
        const auto contents = support::json_string_member(*file, "text");
        REQUIRE(name);
        REQUIRE(contents);
        workspace.write(std::string{*name}, std::string{*contents});
    }
    SECTION("missing confirmed task sidecar is rejected") {
        std::error_code error;
        REQUIRE(std::filesystem::remove(workspace.path() / "task-6.jsonl", error));
        REQUIRE_FALSE(error);
        auto missing = JsonlStorage::open(workspace.path());
        REQUIRE_FALSE(missing);
        CHECK(missing.error().message == "Missing confirmed sidecar record task-6.jsonl at sequence 2");
        return;
    }
    SECTION("mismatched sidecar ordinal is rejected") {
        auto contents = workspace.read("task-6.jsonl");
        const auto position = contents.find("\"ordinal\":0");
        REQUIRE(position != std::string::npos);
        contents.replace(position, std::string{"\"ordinal\":0"}.size(), "\"ordinal\":1");
        workspace.write("task-6.jsonl", contents);
        auto mismatch = JsonlStorage::open(workspace.path());
        REQUIRE_FALSE(mismatch);
        CHECK(mismatch.error().message == "Missing confirmed sidecar record task-6.jsonl at sequence 2");
        return;
    }
    SECTION("mismatched sidecar sequence is rejected") {
        auto contents = workspace.read("task-6.jsonl");
        const auto position = contents.find("\"seq\":2");
        REQUIRE(position != std::string::npos);
        contents.replace(position, std::string{"\"seq\":2"}.size(), "\"seq\":3");
        workspace.write("task-6.jsonl", contents);
        auto mismatch = JsonlStorage::open(workspace.path());
        REQUIRE_FALSE(mismatch);
        CHECK(mismatch.error().message == "Missing confirmed sidecar record task-6.jsonl at sequence 2");
        return;
    }
    SECTION("a confirmed sidecar with a different task id is rejected") {
        auto contents = workspace.read("task-6.jsonl");
        const auto position = contents.find("\"id\":6");
        REQUIRE(position != std::string::npos);
        contents.replace(position, std::string{"\"id\":6"}.size(), "\"id\":9");
        workspace.write("task-6.jsonl", contents);
        auto mismatch = JsonlStorage::open(workspace.path());
        REQUIRE_FALSE(mismatch);
        CHECK(mismatch.error().message == "Confirmed task sidecar data does not match commit 2");
        return;
    }
    SECTION("confirmed task is recovered") {
        auto storage = JsonlStorage::open(workspace.path());
        if (!storage) UNSCOPED_INFO(storage.error().message);
        REQUIRE(storage);
        auto task = (*storage)->task(TaskId{.value = 6});
        REQUIRE(task);
        REQUIRE(task->has_value());
        CHECK((*task)->state.status == "pending");
        REQUIRE((*task)->state.checkpoint);
        CHECK(*(*task)->state.checkpoint == chord::Json{chord::Json::Object{{"phase", "start"}, {"n", 7}}});
        REQUIRE((*storage)->close());
    }
}

// The pi Harness created and closed this pending task. Its frozen phase
// consumes the checkpoint and commits result 17; C++ must execute that work.
TEST_CASE("the C++ Harness continues a pi-created JSONL task", "[durable][jsonl][issue1085]") {
    const char* pi_checkout = std::getenv("PI_CHECKOUT");
    if (pi_checkout == nullptr || *pi_checkout == '\0') SKIP("PI_CHECKOUT is required for frozen pi continuation");
    const std::filesystem::path checkout{pi_checkout};
    TempWorkspace workspace;
    REQUIRE(checkout.string().find_first_of("'\n") == std::string::npos);
    REQUIRE(workspace.path().string().find_first_of("'\n") == std::string::npos);
    const std::string prefix = "NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH='" +
                               (checkout / "tsconfig.json").string() + "' timeout 15 '" +
                               (checkout / "node_modules/.bin/tsx").string() +
                               "' '" CCH_SOURCE_DIR "/docs/research/pi-v1.3.0/continue-1085-harness.mts' '" +
                               workspace.path().string() + "' ";
    REQUIRE(std::system((prefix + "0 create").c_str()) == 0);
    auto ids = support::read_json(workspace.read("pi-created.json"));
    REQUIRE(ids);
    const TaskId task_id{.value = static_cast<std::int64_t>(ids->at("id").get_number())};
    const SubmissionId submission_id{.value = static_cast<std::int64_t>(ids->at("submission").get_number())};
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    auto doc_record = (*storage)->find_document("audit1085.history",
            DocumentAddress{.conversation_id = ConversationId{.value = 1}},
            DocumentPoint::current());
    REQUIRE(doc_record);
    REQUIRE(doc_record->has_value());
    const DocumentId doc_id = (*doc_record)->id;
    auto history = define_document(DocumentDefinitionBuilder{"audit1085.history"}.version(1).conversation(
                                           DocumentHistory::Rewindable, DocumentFork::AsOf),
            [] { return chord::Json{chord::Json::Object{{"n", 0}}}; });
    REQUIRE(history);
    TaskDefinitionBuilder builder{"audit1085.work", 1};
    // History and gate state outlive the Harness, including its close/join.
    builder.phase("start",
            [&history, submission_id](const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
                const chord::Json expected{chord::Json::Object{{"phase", "start"}, {"n", 7}}};
                if (!task.state.checkpoint || *task.state.checkpoint != expected)
                    return support::AsyncResult<void>{std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "unexpected checkpoint"))};
                return runtime.commit([&history, submission_id](Transaction& tx, const TaskRecord& record)
                                              -> support::Expected<std::optional<NextTaskState>> {
                    auto draft = tx.doc(*history, DocumentAddress{.conversation_id = record.conversation_id});
                    if (!draft) return std::unexpected(draft.error());
                    auto changed = draft->set(member_path("n"), chord::Json{2});
                    if (!changed) return std::unexpected(changed.error());
                    auto settled = tx.settle_submission(submission_id,
                            SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                    if (!settled) return std::unexpected(settled.error());
                    return NextTaskState::running(chord::Json{chord::Json::Object{{"phase", "finish"}, {"n", 8}}});
                });
            });
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool reached = false;
    bool released = false;
    builder.phase("finish",
            [&gate_mutex, &gate_cv, &reached, &released](
                    const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                std::unique_lock lock(gate_mutex);
                reached = true;
                gate_cv.notify_all();
                std::stop_callback wake{runtime.stop_token(), [&] { gate_cv.notify_all(); }};
                if (!gate_cv.wait_for(lock, std::chrono::seconds{10}, [&] {
                        return released || runtime.stop_token().stop_requested();
                    }))
                    return support::AsyncResult<void>{
                            std::unexpected(support::make_error(support::ErrorCode::Timeout, "phase gate timed out"))};
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            });
    auto definition = builder.build();
    REQUIRE(definition);
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*definition));
    auto opened = consume(Harness::open(**storage, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    REQUIRE(consume(harness->open_tasks()).has_value());
    REQUIRE(harness->resume());
    bool observed = false;
    {
        std::unique_lock lock(gate_mutex);
        observed = gate_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return reached; });
        if (!observed) {
            released = true;
            gate_cv.notify_all();
        }
    }
    auto closed = consume(harness->close());
    REQUIRE(observed);
    REQUIRE(closed.has_value());
    const auto cpp_markers = json_lines(workspace.read("main.jsonl"));
    REQUIRE(cpp_markers.size() == 4);
    CHECK(cpp_markers[0].at("seq").get_number() == 1);
    CHECK(cpp_markers[1].at("seq").get_number() == 2);
    CHECK(cpp_markers[2].at("seq").get_number() == 3);
    CHECK(cpp_markers[3].at("seq").get_number() == 4);
    const auto cpp_task_lines = json_lines(workspace.read("task-7.jsonl"));
    REQUIRE(cpp_task_lines.size() == 3);
    CHECK(cpp_task_lines[1].at("seq").get_number() == 3);
    CHECK(cpp_task_lines[2].at("seq").get_number() == 4);
    CHECK(cpp_task_lines[2].at("payload").at("value").at("id").get_number() == 7);
    REQUIRE(std::system((prefix + std::to_string(task_id.value) + " finish " + std::to_string(submission_id.value))
                            .c_str()) == 0);
    const auto final_markers = json_lines(workspace.read("main.jsonl"));
    REQUIRE(final_markers.size() == 7);
    for (std::size_t i = 0; i < final_markers.size(); ++i)
        CHECK(final_markers[i].at("seq").get_number() == static_cast<double>(i + 1));
    const auto pi_task_lines = json_lines(workspace.read("pi-task-history.txt"));
    REQUIRE(pi_task_lines.size() == 5);
    CHECK(pi_task_lines[3].at("seq").get_number() == 5);
    CHECK(pi_task_lines[3].at("payload").at("value").at("state").at("status").get_string() == "pending");
    CHECK(pi_task_lines[4].at("seq").get_number() == 6);
    CHECK(pi_task_lines[4].at("payload").at("value").at("state").at("status").get_string() == "running");
    CHECK(final_markers[6].at("writes").get_array().at(0).at("type").get_string() == "task");
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto task = (*reopened)->task(TaskId{.value = 7});
    REQUIRE(task);
    REQUIRE(task->has_value());
    CHECK((*task)->state.status == "terminal");
    REQUIRE((*task)->state.outcome);
    CHECK((*task)->state.outcome->result == chord::Json{17});
    auto current = (*reopened)->document(doc_id, DocumentPoint::current());
    auto original = (*reopened)->document(doc_id, DocumentPoint::as_of(2));
    auto middle = (*reopened)->document(doc_id, DocumentPoint::as_of(4));
    REQUIRE(current);
    REQUIRE(original);
    REQUIRE(middle);
    REQUIRE(current->has_value());
    REQUIRE(original->has_value());
    REQUIRE(middle->has_value());
    CHECK((*current)->value == chord::Json{chord::Json::Object{{"n", 3}}});
    CHECK((*original)->value == chord::Json{chord::Json::Object{{"n", 1}}});
    CHECK((*middle)->value == chord::Json{chord::Json::Object{{"n", 2}}});
    auto final_submission = (*reopened)->submission(submission_id);
    REQUIRE(final_submission);
    REQUIRE(final_submission->has_value());
    CHECK((*final_submission)->status == "unanswered");
    REQUIRE((*reopened)->close());
}

TEST_CASE("a failed C++ task marker recovers for frozen pi continuation", "[durable][jsonl][issue1085]") {
    const char* pi_checkout = std::getenv("PI_CHECKOUT");
    if (pi_checkout == nullptr || *pi_checkout == '\0') SKIP("PI_CHECKOUT is required for frozen pi continuation");
    const std::filesystem::path checkout{pi_checkout};
    TempWorkspace workspace;
    REQUIRE(checkout.string().find_first_of("'\n") == std::string::npos);
    REQUIRE(workspace.path().string().find_first_of("'\n") == std::string::npos);
    const std::string prefix = "NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH='" +
                               (checkout / "tsconfig.json").string() + "' timeout 15 '" +
                               (checkout / "node_modules/.bin/tsx").string() +
                               "' '" CCH_SOURCE_DIR "/docs/research/pi-v1.3.0/continue-1085-harness.mts' '" +
                               workspace.path().string() + "' ";
    REQUIRE(std::system((prefix + "0 create").c_str()) == 0);
    auto ids = support::read_json(workspace.read("pi-created.json"));
    REQUIRE(ids);
    const TaskId id{.value = static_cast<std::int64_t>(ids->at("id").get_number())};
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    auto original = (*storage)->task(id);
    REQUIRE(original);
    REQUIRE(original->has_value());
    const auto main = workspace.read("main.jsonl");
    const auto task_file = "task-" + std::to_string(id.value) + ".jsonl";
    const auto sidecar = workspace.read(task_file);
    std::error_code path_error;
    REQUIRE(std::filesystem::remove(workspace.path() / "main.jsonl", path_error));
    REQUIRE_FALSE(path_error);
    REQUIRE(std::filesystem::create_directory(workspace.path() / "main.jsonl", path_error));
    REQUIRE_FALSE(path_error);
    auto candidate = **original;
    candidate.state =
            TaskState{.status = "running", .checkpoint = chord::Json{chord::Json::Object{{"phase", "wrong"}}}};
    const std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = candidate}};
    auto failed = consume((*storage)->commit(writes));
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message == "JSONL storage is poisoned and must be reopened");
    CHECK(workspace.read(task_file).size() > sidecar.size());
    REQUIRE(std::filesystem::remove(workspace.path() / "main.jsonl", path_error));
    REQUIRE_FALSE(path_error);
    workspace.write("main.jsonl", main);
    auto recovered = JsonlStorage::open(workspace.path());
    REQUIRE(recovered);
    auto restored = (*recovered)->task(id);
    REQUIRE(restored);
    REQUIRE(restored->has_value());
    CHECK((*restored)->state.status == "pending");
    CHECK((*restored)->state.checkpoint == (*original)->state.checkpoint);
    CHECK(workspace.read(task_file) == sidecar);
    REQUIRE((*recovered)->close());
    REQUIRE(std::system((prefix + std::to_string(id.value) + " advance").c_str()) == 0);
    auto final_storage = JsonlStorage::open(workspace.path());
    REQUIRE(final_storage);
    auto continued = (*final_storage)->task(id);
    REQUIRE(continued);
    REQUIRE(continued->has_value());
    CHECK((*continued)->state.checkpoint == chord::Json{chord::Json::Object{{"phase", "finish"}, {"n", 8}}});
    REQUIRE((*final_storage)->close());
}

TEST_CASE("JSONL recovery rejects a task sidecar symlink without changing its target", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    TempWorkspace outside;
    outside.write("user-data", "complete\npartial");
    std::error_code error;
    std::filesystem::create_symlink(outside.path() / "user-data", store.path() / "task-2.jsonl", error);
    REQUIRE_FALSE(error);
    auto opened = JsonlStorage::open(store.path());
    CHECK_FALSE(opened.has_value());
    CHECK(outside.read("user-data") == "complete\npartial");
}

TEST_CASE("JSONL recovery preserves a hard-linked external file", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    TempWorkspace outside;
    outside.write("user-data", "complete\npartial");
    std::error_code error;
    std::filesystem::create_hard_link(outside.path() / "user-data", store.path() / "task-2.jsonl", error);
    REQUIRE_FALSE(error);
    auto opened = JsonlStorage::open(store.path());
    CHECK_FALSE(opened.has_value());
    CHECK(outside.read("user-data") == "complete\npartial");
}

TEST_CASE("JSONL task append rejects external file aliases", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    TempWorkspace outside;
    outside.write("user-data", "untouched");
    auto opened = JsonlStorage::open(store.path());
    REQUIRE(opened);
    std::error_code error;
    SECTION("symlink") {
        std::filesystem::create_symlink(outside.path() / "user-data", store.path() / "task-2.jsonl", error);
    }
    SECTION("hard link") {
        std::filesystem::create_hard_link(outside.path() / "user-data", store.path() / "task-2.jsonl", error);
    }
    REQUIRE_FALSE(error);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    const std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    auto result = consume((*opened)->commit(writes));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "JSONL storage is poisoned and must be reopened");
    CHECK(outside.read("user-data") == "untouched");
}

TEST_CASE("JSONL terminal reclamation keeps an external sidecar through a replaced store directory",
        "[durable][jsonl][issue1085]") {
    TempWorkspace workspace;
    const auto store = workspace.path() / "store";
    const auto moved = workspace.path() / "store-moved";
    const auto external = workspace.path() / "external";
    std::error_code error;
    std::filesystem::create_directories(store, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_directories(external, error);
    REQUIRE_FALSE(error);
    auto storage = JsonlStorage::open(store);
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    REQUIRE(std::filesystem::exists(store / "task-2.jsonl", error));
    REQUIRE_FALSE(error);

    // The store path is replaced after the storage captured its directory.
    // Reclamation must stay in the captured directory, not resolve this path.
    std::filesystem::rename(store, moved, error);
    REQUIRE_FALSE(error);
    std::filesystem::create_symlink(external, store, error);
    REQUIRE_FALSE(error);
    workspace.write("external/task-2.jsonl", "outside\n");

    task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    writes = {StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    CHECK(workspace.read("external/task-2.jsonl") == "outside\n");
    CHECK_FALSE(std::filesystem::exists(moved / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    REQUIRE((*storage)->close());
    std::filesystem::remove(store, error);
    REQUIRE_FALSE(error);
}

TEST_CASE("JSONL terminal reclamation leaves a substituted sidecar entry intact", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    TempWorkspace outside;
    outside.write("user-data", "untouched");
    auto storage = JsonlStorage::open(store.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    std::error_code error;
    REQUIRE(std::filesystem::remove(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    SECTION("symlink") {
        std::filesystem::create_symlink(outside.path() / "user-data", store.path() / "task-2.jsonl", error);
    }
    SECTION("hard link") {
        std::filesystem::create_hard_link(outside.path() / "user-data", store.path() / "task-2.jsonl", error);
    }
    SECTION("replaced regular file") { store.write("task-2.jsonl", "replacement\n"); }
    REQUIRE_FALSE(error);
    task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    writes = {StorageWrite{.type = "task", .task = task}};
    // Publication succeeds; the substituted entry is unknown, so it is neither
    // written through nor unlinked.
    auto result = consume((*storage)->commit(writes));
    REQUIRE(result.has_value());
    CHECK(outside.read("user-data") == "untouched");
    CHECK(std::filesystem::exists(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    REQUIRE((*storage)->close());
}

TEST_CASE("JSONL task update rejects a replaced sidecar identity", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    auto storage = JsonlStorage::open(store.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    std::error_code error;
    std::filesystem::rename(store.path() / "task-2.jsonl", store.path() / "task-2.user", error);
    REQUIRE_FALSE(error);
    store.write("task-2.jsonl", "user-data\n");
    task.state.checkpoint = chord::Json{chord::Json::Object{{"phase", "again"}}};
    writes = {StorageWrite{.type = "task", .task = task}};
    auto result = consume((*storage)->commit(writes));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "JSONL storage is poisoned and must be reopened");
    CHECK(store.read("task-2.jsonl") == "user-data\n");
    REQUIRE((*storage)->close());
}

namespace {

[[nodiscard]] std::size_t open_descriptor_count() {
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator iterator("/proc/self/fd", error);
            !error && iterator != std::filesystem::directory_iterator{};
            iterator.increment(error)) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("JSONL recovery opens one task sidecar at a time", "[durable][jsonl][issue1085]") {
    struct rlimit original{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    // Keep the descriptor budget well below the sidecar count so retaining one
    // descriptor per sidecar fails deterministically instead of by host limit.
    const rlim_t budget = static_cast<rlim_t>(open_descriptor_count() + 32);
    bool limit_lowered = true;
    if (original.rlim_cur > budget) {
        struct rlimit reduced = original;
        reduced.rlim_cur = budget;
        limit_lowered = ::setrlimit(RLIMIT_NOFILE, &reduced) == 0;
    }

    // A failed assertion does not unwind under the strict no-exception build,
    // so everything until the restore below records outcomes instead of asserting.
    TempWorkspace store;
    constexpr std::size_t kSidecars = 64;
    bool created = true;
    for (std::size_t index = 0; index < kSidecars; ++index) {
        std::ofstream sidecar(store.path() / ("task-" + std::to_string(index + 2) + ".jsonl"), std::ios::binary);
        created = sidecar.good() && created;
    }
    auto storage = JsonlStorage::open(store.path());
    const bool opened = storage.has_value();
    const bool closed = opened && (*storage)->close().has_value();
    std::error_code error;
    std::size_t remaining = 0;
    for (const auto& entry : std::filesystem::directory_iterator(store.path(), error)) {
        if (entry.path().filename().string().starts_with("task-")) {
            ++remaining;
        }
    }
    const bool listed = !error;

    // Release the process-wide limit before asserting, whatever the outcome.
    (void)::setrlimit(RLIMIT_NOFILE, &original);

    REQUIRE(limit_lowered);
    REQUIRE(created);
    REQUIRE(opened);
    REQUIRE(closed);
    REQUIRE(listed);
    CHECK(remaining == 0);
}

TEST_CASE("JSONL recovery tolerates a failed task sidecar reclamation", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    auto storage = JsonlStorage::open(store.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    {
        std::ofstream probe(store.path() / "reclaim-probe", std::ios::binary);
        REQUIRE(probe.good());
    }

    // Reclamation needs write permission on the directory; publication does not.
    // Root or CAP_DAC_OVERRIDE bypasses the mode, so probe the real syscall and
    // skip rather than assert on a permissive host.
    std::error_code error;
    std::filesystem::permissions(store.path(),
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
                    std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace,
            error);
    REQUIRE_FALSE(error);

    // No assertion may run before the permissions are restored: a failed
    // REQUIRE does not unwind under the strict no-exception build, which would
    // leave the directory unreadable to the workspace cleanup.
    errno = 0;
    const bool unlink_denied =
            ::unlink((store.path() / "reclaim-probe").c_str()) != 0 && (errno == EACCES || errno == EPERM);
    task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    writes = {StorageWrite{.type = "task", .task = task}};
    const bool committed = consume((*storage)->commit(writes)).has_value();
    const bool closed = (*storage)->close().has_value();
    std::error_code exists_error;
    const bool sidecar_present = std::filesystem::exists(store.path() / "task-2.jsonl", exists_error);
    auto reopened = JsonlStorage::open(store.path());
    const bool opened = reopened.has_value();
    bool terminal = false;
    bool reopened_closed = false;
    if (opened) {
        auto recovered = (*reopened)->task(task.id);
        terminal = recovered.has_value() && recovered->has_value() && (*recovered)->state.status == "terminal";
        reopened_closed = (*reopened)->close().has_value();
    }

    std::error_code restore_error;
    std::filesystem::permissions(
            store.path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, restore_error);
    REQUIRE_FALSE(restore_error);
    if (!unlink_denied) {
        SKIP("unlink permission denial is unavailable for this process; skipping the reclamation fixture");
    }

    REQUIRE(committed);
    REQUIRE(closed);
    CHECK(sidecar_present);
    REQUIRE(opened);
    CHECK(terminal);
    REQUIRE(reopened_closed);
}

TEST_CASE("a reclaimed empty task sidecar allows a later task commit", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    {
        std::ofstream sidecar(store.path() / "task-2.jsonl", std::ios::binary);
        REQUIRE(sidecar.good());
    }
    auto storage = JsonlStorage::open(store.path());
    REQUIRE(storage);
    std::error_code error;
    CHECK_FALSE(std::filesystem::exists(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);

    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    const std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    REQUIRE((*storage)->close());

    auto reopened = JsonlStorage::open(store.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(task.id);
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.status == "pending");
    REQUIRE((*reopened)->close());
}

TEST_CASE("a task commit rejects a sidecar that reappears after reclamation", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    {
        std::ofstream sidecar(store.path() / "task-2.jsonl", std::ios::binary);
        REQUIRE(sidecar.good());
    }
    auto storage = JsonlStorage::open(store.path());
    REQUIRE(storage);
    std::error_code error;
    REQUIRE_FALSE(std::filesystem::exists(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    {
        std::ofstream external(store.path() / "task-2.jsonl", std::ios::binary);
        external << "external\n";
        REQUIRE(external.good());
    }

    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    const std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    CHECK_FALSE(consume((*storage)->commit(writes)).has_value());
    CHECK(store.read("task-2.jsonl") == "external\n");
    REQUIRE((*storage)->close());
}

TEST_CASE("a vanished task sidecar is not recreated by a live update", "[durable][jsonl][issue1085]") {
    TempWorkspace store;
    auto storage = JsonlStorage::open(store.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    std::error_code error;
    REQUIRE(std::filesystem::exists(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::remove(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);

    task.state.checkpoint = chord::Json{chord::Json::Object{{"phase", "next"}}};
    writes = {StorageWrite{.type = "task", .task = task}};
    CHECK_FALSE(consume((*storage)->commit(writes)).has_value());
    error.clear();
    CHECK_FALSE(std::filesystem::exists(store.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    REQUIRE((*storage)->close());
}

TEST_CASE("an unconfirmed live task sidecar is discarded on JSONL reopen", "[durable][jsonl][issue1085]") {
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    const std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    REQUIRE((*storage)->close());
    const auto original = workspace.read("task-2.jsonl");
    const auto lines = json_lines(original);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].at("seq").get_number() == 1);
    CHECK(lines[0].at("ordinal").get_number() == 0);
    auto unconfirmed = original;
    const auto pos = unconfirmed.find("\"seq\":1");
    REQUIRE(pos != std::string::npos);
    unconfirmed.replace(pos, std::string{"\"seq\":1"}.size(), "\"seq\":2");
    append_text(workspace.path() / "task-2.jsonl", unconfirmed);
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(TaskId{.value = 2});
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.status == "pending");
    CHECK(workspace.read("task-2.jsonl") == original);
    REQUIRE((*reopened)->close());
}

TEST_CASE("a JSONL batch preserves ordered task updates and terminal reclamation", "[durable][jsonl][issue1085]") {
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskRecord first{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state = TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "one"}}}}};
    TaskRecord second = first;
    second.state.checkpoint = chord::Json{chord::Json::Object{{"phase", "two"}}};
    const std::vector<StorageWrite> writes{
            StorageWrite{.type = "task", .task = first}, StorageWrite{.type = "task", .task = second}};
    auto result = consume((*storage)->commit(writes));
    REQUIRE(result.has_value());
    const auto sidecar = json_lines(workspace.read("task-2.jsonl"));
    REQUIRE(sidecar.size() == 2);
    CHECK(sidecar[0].at("ordinal").get_number() == 0);
    CHECK(sidecar[1].at("ordinal").get_number() == 1);
    REQUIRE((*storage)->close());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(first.id);
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.checkpoint == second.state.checkpoint);
    auto terminal = second;
    terminal.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    const std::vector<StorageWrite> final_batch{
            StorageWrite{.type = "task", .task = second}, StorageWrite{.type = "task", .task = terminal}};
    REQUIRE(consume((*reopened)->commit(final_batch)).has_value());
    std::error_code exists_error;
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "task-2.jsonl", exists_error));
    REQUIRE_FALSE(exists_error);
    REQUIRE((*reopened)->close());
    auto final_storage = JsonlStorage::open(workspace.path());
    REQUIRE(final_storage);
    auto final_task = (*final_storage)->task(first.id);
    REQUIRE(final_task);
    REQUIRE(final_task->has_value());
    CHECK((*final_task)->state.status == "terminal");
    REQUIRE((*final_storage)->close());
}

TEST_CASE("a terminal then live task batch is accepted with the last write winning",
        "[durable][jsonl][issue1085][issue1168]") {
    // Issue #1168: frozen pi v1.3.0 public Storage accepts a terminal then
    // live task batch with the last write winning (frozen `audit-1168`); the
    // terminal-replace invariant lives in the frozen Session transaction,
    // not in Storage. This is the JSONL end of that contract.
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskRecord terminal{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}}};
    TaskRecord live = terminal;
    live.state = TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "again"}}}};
    const std::vector<StorageWrite> writes{
            StorageWrite{.type = "task", .task = terminal}, StorageWrite{.type = "task", .task = live}};
    auto result = consume((*storage)->commit(writes));
    REQUIRE(result.has_value());
    auto task = (*storage)->task(live.id);
    REQUIRE(task);
    REQUIRE(task->has_value());
    CHECK((*task)->state.status == "pending");
    std::error_code error;
    REQUIRE(std::filesystem::exists(workspace.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    REQUIRE((*storage)->close());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(live.id);
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.status == "pending");
    REQUIRE((*reopened)->close());
}

TEST_CASE("terminal task publication reclaims its JSONL sidecar", "[durable][jsonl][issue1085]") {
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    std::error_code exists_error;
    REQUIRE(std::filesystem::exists(workspace.path() / "task-2.jsonl", exists_error));
    REQUIRE_FALSE(exists_error);
    task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    writes = {StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "task-2.jsonl", exists_error));
    REQUIRE_FALSE(exists_error);
    REQUIRE((*storage)->close());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(TaskId{.value = 2});
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.status == "terminal");
    REQUIRE((*reopened)->close());
}

TEST_CASE("failed task sidecar reclamation leaves a published terminal result", "[durable][jsonl][issue1085]") {
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    std::error_code error;
    REQUIRE(std::filesystem::remove(workspace.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::create_directory(workspace.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    workspace.write("task-2.jsonl/hold", "x");
    task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    writes = {StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    // remove(task-2.jsonl) fails with ENOTEMPTY; the unrelated child survives.
    CHECK(workspace.read("task-2.jsonl/hold") == "x");
    auto committed = (*storage)->task(task.id);
    REQUIRE(committed);
    REQUIRE(committed->has_value());
    CHECK((*committed)->state.status == "terminal");
    REQUIRE((*storage)->close());
    REQUIRE(std::filesystem::remove(workspace.path() / "task-2.jsonl/hold", error));
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::remove(workspace.path() / "task-2.jsonl", error));
    REQUIRE_FALSE(error);
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(task.id);
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.status == "terminal");
    REQUIRE((*reopened)->close());
}

TEST_CASE("a failed terminal marker preserves the live JSONL task on reopen", "[durable][jsonl][issue1085]") {
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskRecord task{.id = TaskId{.value = 2},
            .kind = "work",
            .version = 1,
            .state =
                    TaskState{.status = "pending", .checkpoint = chord::Json{chord::Json::Object{{"phase", "start"}}}}};
    std::vector<StorageWrite> writes{StorageWrite{.type = "task", .task = task}};
    REQUIRE(consume((*storage)->commit(writes)).has_value());
    const auto main_before = workspace.read("main.jsonl");
    const auto sidecar_before = workspace.read("task-2.jsonl");
    std::error_code path_error;
    REQUIRE(std::filesystem::remove(workspace.path() / "main.jsonl", path_error));
    REQUIRE_FALSE(path_error);
    REQUIRE(std::filesystem::create_directory(workspace.path() / "main.jsonl", path_error));
    REQUIRE_FALSE(path_error);
    task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
    writes = {StorageWrite{.type = "task", .task = task}};
    const auto result = consume((*storage)->commit(writes));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "JSONL storage is poisoned and must be reopened");
    CHECK(workspace.read("task-2.jsonl") == sidecar_before);
    REQUIRE(std::filesystem::remove(workspace.path() / "main.jsonl", path_error));
    REQUIRE_FALSE(path_error);
    workspace.write("main.jsonl", main_before);
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    auto recovered = (*reopened)->task(TaskId{.value = 2});
    REQUIRE(recovered);
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->state.status == "pending");
    REQUIRE((*reopened)->close());
}

// Pi must open and execute the C++-created pending task, not merely inspect
// a C++ snapshot. The child process is bounded even on a stalled task.
TEST_CASE("frozen pi continues a C++ created JSONL task", "[durable][jsonl][issue1085]") {
    const char* pi_checkout = std::getenv("PI_CHECKOUT");
    if (pi_checkout == nullptr || *pi_checkout == '\0') SKIP("PI_CHECKOUT is required for frozen pi continuation");
    TempWorkspace workspace;
    auto storage = JsonlStorage::open(workspace.path());
    REQUIRE(storage);
    TaskDefinitionBuilder initial_builder{"audit1085.work", 1};
    initial_builder.initial(
            [](const chord::Json&) { return chord::Json{chord::Json::Object{{"phase", "start"}, {"n", 7}}}; });
    initial_builder.phase("start", [](const TaskRecord&, TaskRuntime&) -> support::AsyncResult<void> {
        return support::AsyncResult<void>{
                std::unexpected(support::make_error(support::ErrorCode::Validation, "must run in pi"))};
    });
    auto initial_definition = initial_builder.build();
    REQUIRE(initial_definition);
    std::vector<TaskDefinition> initial_tasks;
    initial_tasks.push_back(std::move(*initial_definition));
    auto initial_opened = consume(Harness::open(**storage, HarnessOptions{.tasks = std::move(initial_tasks)}));
    REQUIRE(initial_opened.has_value());
    auto initial = std::move(initial_opened.value());
    auto root = consume(initial->root());
    REQUIRE(root.has_value());
    auto history = define_document(DocumentDefinitionBuilder{"audit1085.history"}.version(1).conversation(
                                           DocumentHistory::Rewindable, DocumentFork::AsOf),
            [] { return chord::Json{chord::Json::Object{{"n", 0}}}; });
    REQUIRE(history);
    TaskId task_id{};
    SubmissionId submission_id{};
    DocumentId document_id{};
    REQUIRE(consume(initial->session().commit([&](Transaction& tx) -> support::ExpectedVoid {
        auto created = tx.create_task(*initial->task_definition("audit1085.work"),
                chord::Json{chord::Json::Object{}},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        auto submission = tx.create_submission(SubmissionRecord{.conversation_id = root.value(),
                .request_id = std::string{"audit1085-request"},
                .type = "input",
                .status = "queued"});
        if (!submission) return std::unexpected(submission.error());
        submission_id = submission->id;
        auto draft = tx.doc(*history, DocumentAddress{.conversation_id = root.value()});
        if (!draft) return std::unexpected(draft.error());
        auto set = draft->set(member_path("n"), chord::Json{1});
        if (!set) return std::unexpected(set.error());
        return {};
    })).has_value());
    auto document_record = (*storage)->find_document(
            "audit1085.history", DocumentAddress{.conversation_id = root.value()}, DocumentPoint::current());
    REQUIRE(document_record);
    REQUIRE(document_record->has_value());
    document_id = (*document_record)->id;
    REQUIRE(consume(initial->close()).has_value());
    const std::filesystem::path checkout{pi_checkout};
    REQUIRE(checkout.string().find_first_of("'\n") == std::string::npos);
    REQUIRE(workspace.path().string().find_first_of("'\n") == std::string::npos);
    const std::string command =
            "NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH='" + (checkout / "tsconfig.json").string() +
            "' timeout 15 '" + (checkout / "node_modules/.bin/tsx").string() +
            "' '" CCH_SOURCE_DIR "/docs/research/pi-v1.3.0/continue-1085-harness.mts' '" + workspace.path().string() +
            "' " + std::to_string(task_id.value) + " advance " + std::to_string(submission_id.value);
    REQUIRE(std::system(command.c_str()) == 0);
    const auto pi_history = json_lines(workspace.read("doc-" + std::to_string(document_id.value) + ".jsonl"));
    REQUIRE(pi_history.size() == 2);
    const auto pi_document_seq = static_cast<Seq>(pi_history.back().at("seq").get_number());
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE(reopened);
    TaskDefinitionBuilder builder{"audit1085.work", 1};
    // The definition and root remain alive until the Harness has closed.
    builder.phase(
            "finish", [&history, &root](const TaskRecord& record, TaskRuntime& runtime) -> support::AsyncResult<void> {
                if (!record.state.checkpoint ||
                        *record.state.checkpoint != chord::Json{chord::Json::Object{{"phase", "finish"}, {"n", 8}}})
                    return support::AsyncResult<void>{std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "unexpected checkpoint"))};
                return runtime.commit([&history, &root](Transaction& tx,
                                              const TaskRecord&) -> support::Expected<std::optional<NextTaskState>> {
                    auto draft = tx.doc(*history, DocumentAddress{.conversation_id = root.value()});
                    if (!draft) return std::unexpected(draft.error());
                    auto changed = draft->set(member_path("n"), chord::Json{3});
                    if (!changed) return std::unexpected(changed.error());
                    return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json{17}});
                });
            });
    auto definition = builder.build();
    REQUIRE(definition);
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*definition));
    auto opened = consume(Harness::open(**reopened, HarnessOptions{.tasks = std::move(tasks)}));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    REQUIRE(consume(harness->open_tasks()).has_value());
    REQUIRE(harness->resume());
    auto receipt = consume(harness->wait_for_task(task_id));
    REQUIRE(receipt.has_value());
    REQUIRE(receipt.value().state.outcome);
    CHECK(receipt.value().state.outcome->result == chord::Json{17});
    auto submission = (*reopened)->submission(submission_id);
    REQUIRE(submission);
    REQUIRE(submission->has_value());
    CHECK((*submission)->status == "unanswered");
    CHECK((*submission)->reason == std::optional<std::string>{"aborted"});
    CHECK((*submission)->request_id == std::optional<std::string>{"audit1085-request"});
    auto current_doc = (*reopened)->document(document_id, DocumentPoint::current());
    REQUIRE(current_doc);
    REQUIRE(current_doc->has_value());
    CHECK((*current_doc)->value == chord::Json{chord::Json::Object{{"n", 3}}});
    auto pi_doc = (*reopened)->document(document_id, DocumentPoint::as_of(pi_document_seq));
    REQUIRE(pi_doc);
    REQUIRE(pi_doc->has_value());
    CHECK((*pi_doc)->value == chord::Json{chord::Json::Object{{"n", 2}}});
    auto original_doc = (*reopened)->document(document_id, DocumentPoint::as_of(2));
    REQUIRE(original_doc);
    REQUIRE(original_doc->has_value());
    CHECK((*original_doc)->value == chord::Json{chord::Json::Object{{"n", 1}}});
    REQUIRE(consume(harness->close()).has_value());
    auto final_storage = JsonlStorage::open(workspace.path());
    REQUIRE(final_storage);
    auto final_doc = (*final_storage)->document(document_id, DocumentPoint::current());
    REQUIRE(final_doc);
    REQUIRE(final_doc->has_value());
    CHECK((*final_doc)->value == chord::Json{chord::Json::Object{{"n", 3}}});
    auto historical_doc = (*final_storage)->document(document_id, DocumentPoint::as_of(pi_document_seq));
    REQUIRE(historical_doc);
    REQUIRE(historical_doc->has_value());
    CHECK((*historical_doc)->value == chord::Json{chord::Json::Object{{"n", 2}}});
    auto final_submission = (*final_storage)->submission(submission_id);
    REQUIRE(final_submission);
    REQUIRE(final_submission->has_value());
    CHECK((*final_submission)->status == "unanswered");
    REQUIRE((*final_storage)->close());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "a JSONL commit appends every sidecar before the main marker".
//
// Discriminating case: the marker carries the sidecar's sequence and ordinal,
// and the sidecar carries the base revision the marker names. A backend that
// writes only a marker, or that names no matching sidecar record, fails the
// decoded comparison even though the files exist.
TEST_CASE("a JSONL store commits, closes and reopens the same records through the public storage contract",
        "[durable][jsonl][commit][reopen][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kOrderScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;

    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);

    const ConversationId conversation_id = create_conversation(*session);
    CHECK(conversation_id.value == 2);
    const auto definition = latest_document();
    std::optional<EntryId> entry_id;
    std::optional<DocumentId> document_id;
    auto commit = run_commit(*session,
            [&conversation_id, &definition, &entry_id, &document_id](
                    Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(
                        conversation_id, "audit1054.note", chord::Json{chord::Json::Object{{"n", chord::Json{1}}}});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                entry_id = entry->id;
                auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                if (!draft) {
                    return std::unexpected(std::move(draft.error()));
                }
                if (auto set = draft->set(member_path("text"), chord::Json{std::string{"ordered"}}); !set) {
                    return std::unexpected(std::move(set.error()));
                }
                if (auto set = draft->set(member_path("revision"), chord::Json{1.0}); !set) {
                    return std::unexpected(std::move(set.error()));
                }
                return {};
            });
    REQUIRE(commit.has_value());
    REQUIRE(entry_id.has_value());
    REQUIRE(entry_id->value == 3);

    // The decoded marker and sidecar record are the frozen ones. Field order
    // is not part of a JSON value; the frozen artifact's own objects are the
    // comparison source.
    const auto main_lines = json_lines(workspace.read("main.jsonl"));
    REQUIRE(main_lines.size() == 2);
    const auto* main_object = main_lines.back().get_if<support::JsonValue::object_t>();
    REQUIRE(main_object != nullptr);
    const auto* writes = support::json_array_member(*main_object, "writes");
    REQUIRE(writes != nullptr);
    REQUIRE(writes->size() == 2);
    const auto* document_create = (*writes)[1].get_if<support::JsonValue::object_t>();
    REQUIRE(document_create != nullptr);
    const auto* record = support::json_object_member(*document_create, "record");
    REQUIRE(record != nullptr);
    const auto frozen_document_id = support::json_integer_member(*record, "id");
    REQUIRE(frozen_document_id.has_value());
    document_id = DocumentId{.value = *frozen_document_id};
    CHECK(*document_id == DocumentId{.value = 4});

    const auto* frozen_marker = scenario.object_member("mainMarker");
    REQUIRE(frozen_marker != nullptr);
    CHECK(canonical_text(main_lines.back()) == canonical_text(support::JsonValue{*frozen_marker}));

    const auto sidecar_lines = json_lines(workspace.read("doc-4.jsonl"));
    REQUIRE(sidecar_lines.size() == 1);
    const auto* frozen_sidecar = scenario.object_member("sidecarRecord");
    REQUIRE(frozen_sidecar != nullptr);
    CHECK(canonical_text(sidecar_lines.back()) == canonical_text(support::JsonValue{*frozen_sidecar}));

    // Close and reopen through the same public seam: the records and the ID
    // namespace continue rather than restarting.
    REQUIRE(consume(session->close()).has_value());
    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);

    std::optional<ConversationRecord> reopened_conversation;
    std::optional<EntryRecord> reopened_entry;
    REQUIRE(run_commit(*reopened,
            [&conversation_id, &entry_id, &reopened_conversation, &reopened_entry](
                    Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.conversation(conversation_id);
                if (!conversation) {
                    return std::unexpected(std::move(conversation.error()));
                }
                if ((*conversation).has_value()) {
                    reopened_conversation = **conversation;
                }
                auto entry = transaction.entry(*entry_id);
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                if ((*entry).has_value()) {
                    reopened_entry = **entry;
                }
                return {};
            }).has_value());
    REQUIRE(reopened_conversation.has_value());
    CHECK(reopened_conversation->id == conversation_id);
    REQUIRE(reopened_entry.has_value());
    CHECK(reopened_entry->id == *entry_id);
    CHECK(reopened_entry->kind == "audit1054.note");

    auto snapshot = consume(reopened->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(*snapshot.value() == chord::Json{chord::Json::Object{
                                       {"text", chord::Json{std::string{"ordered"}}}, {"revision", chord::Json{1.0}}}});

    std::optional<EntryId> continued_entry;
    REQUIRE(run_commit(
            *reopened, [&conversation_id, &continued_entry](Transaction& transaction) -> support::Expected<void> {
                auto entry = transaction.append_entry(conversation_id, "audit1054.note", chord::Json{2.0});
                if (!entry) {
                    return std::unexpected(std::move(entry.error()));
                }
                continued_entry = entry->id;
                return {};
            }).has_value());
    REQUIRE(continued_entry.has_value());
    CHECK(continued_entry->value > document_id->value);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail".
//
// Discriminating case: the unconfirmed sidecar record is complete and names
// sequence 4, while the last confirmed marker is sequence 2. Recovery must
// compare those sequences, drop the tail, and restore both files to their
// pre-crash bytes. A backend that adopts the complete sidecar tail, or keeps
// the torn marker, fails the file comparison and the snapshot.
TEST_CASE("JSONL recovery truncates a torn main marker and an unconfirmed sidecar tail",
        "[durable][jsonl][recovery][torn][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kRecoveryScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;

    TempWorkspace workspace;
    {
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        auto storage = std::move(*opened);
        auto session = Session::create(*storage);
        const ConversationId conversation_id = create_conversation(*session);
        const auto definition = latest_document();
        REQUIRE(run_commit(
                *session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                    auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                    if (!draft) {
                        return std::unexpected(std::move(draft.error()));
                    }
                    return draft->set(member_path("text"), chord::Json{std::string{"kept"}});
                }).has_value());
        REQUIRE(consume(session->close()).has_value());
    }

    const std::string main_before = workspace.read("main.jsonl");
    const std::string sidecar_before = workspace.read("doc-3.jsonl");
    const auto confirmed_main = json_lines(main_before);
    const auto confirmed_sidecar = json_lines(sidecar_before);
    REQUIRE(confirmed_main.size() == 2);
    REQUIRE(confirmed_sidecar.size() == 1);
    const auto confirmed_main_seq =
            support::json_integer_member(confirmed_main.back().get<support::JsonValue::object_t>(), "seq");
    const auto confirmed_sidecar_seq =
            support::json_integer_member(confirmed_sidecar.back().get<support::JsonValue::object_t>(), "seq");
    REQUIRE(confirmed_main_seq.has_value());
    REQUIRE(confirmed_sidecar_seq.has_value());
    CHECK(*confirmed_main_seq == 2);
    CHECK(*confirmed_sidecar_seq == 2);

    // The captured crash shape: a torn main marker plus a complete but
    // unconfirmed sidecar record.
    append_text(workspace.path() / "main.jsonl", R"({"format":1,"type":"commit","seq":3,"writes":[)");
    const support::JsonValue unconfirmed = json_object({
            {"format", support::JsonValue{1}},
            {"type", support::JsonValue{"record"}},
            {"seq", support::JsonValue{4}},
            {"ordinal", support::JsonValue{0}},
            {"payload",
                    json_object({
                            {"type", support::JsonValue{"document"}},
                            {"id", support::JsonValue{3}},
                            {"content",
                                    json_object({
                                            {"version", support::JsonValue{1}},
                                            {"kind", support::JsonValue{"delta"}},
                                            {"ops",
                                                    support::JsonValue{support::JsonValue::array_t{support::JsonValue{
                                                            support::JsonValue::array_t{support::JsonValue{"s"},
                                                                    support::JsonValue{support::JsonValue::array_t{
                                                                            support::JsonValue{"text"}}},
                                                                    support::JsonValue{"unconfirmed"}}}}}},
                                    })},
                    })},
    });
    append_text(workspace.path() / "doc-3.jsonl", json_text(unconfirmed) + "\n");
    CHECK(support::json_integer_member(unconfirmed.get<support::JsonValue::object_t>(), "seq").value_or(0) >
            *confirmed_main_seq);

    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);
    const auto definition = latest_document();
    auto snapshot = consume(reopened->snapshot(definition, DocumentAddress{.conversation_id = ConversationId{2}}));
    REQUIRE(snapshot.has_value());
    const auto* frozen_recovered = scenario.member("recovered");
    REQUIRE(frozen_recovered != nullptr);
    CHECK(json_equals(*snapshot.value(), *frozen_recovered));

    // Recovery restores both files byte-for-byte to their pre-crash content.
    CHECK(workspace.read("main.jsonl") == main_before);
    CHECK(workspace.read("doc-3.jsonl") == sidecar_before);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "JSONL refuses a main marker whose confirmed sidecar record is missing".
//
// Discriminating case: a marker at sequence 2 names doc-3.jsonl at ordinal 0,
// and that confirmed record is deleted. A permissive reader that drops the
// document passes a happy reopen; the frozen corruption message separates it.
TEST_CASE("JSONL refuses a main marker whose confirmed sidecar record is missing",
        "[durable][jsonl][recovery][corruption][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kMissingScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto frozen_sidecar = scenario.string_member("missingSidecar");
    REQUIRE(frozen_sidecar.has_value());
    const auto* frozen_open = scenario.object_member("open");
    REQUIRE(frozen_open != nullptr);
    const auto* frozen_message = support::json_member(*frozen_open, "message");
    REQUIRE(frozen_message != nullptr);
    const std::string expected_message = frozen_message->get<std::string>();
    CHECK(expected_message == "Missing confirmed sidecar record doc-3.jsonl at sequence 2");

    TempWorkspace workspace;
    {
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        auto storage = std::move(*opened);
        auto session = Session::create(*storage);
        const ConversationId conversation_id = create_conversation(*session);
        const auto definition = latest_document();
        REQUIRE(run_commit(
                *session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                    auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                    if (!draft) {
                        return std::unexpected(std::move(draft.error()));
                    }
                    return draft->set(member_path("text"), chord::Json{std::string{"x"}});
                }).has_value());
        REQUIRE(consume(session->close()).has_value());
    }

    std::filesystem::remove(workspace.path() / *frozen_sidecar);
    auto reopened = JsonlStorage::open(workspace.path());
    REQUIRE_FALSE(reopened.has_value());
    CHECK(reopened.error().message == expected_message);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "a JSONL main-marker I/O failure poisons the instance while reopen recovers
// the published prefix".
//
// Discriminating case: the commit fails at the public file path, then the
// original instance must reject both writes and reads while only a reopen
// recovers the published prefix. A backend that retries in place, stays
// usable, or replays the failed batch fails one of those observations.
//
// This models an injected append failure, not SIGKILL or power loss: no fsync
// or crash-consistency guarantee is claimed from it.
TEST_CASE("a JSONL main-marker I/O failure poisons the store while reopen recovers the published prefix",
        "[durable][jsonl][poison][recovery][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kPoisonScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto* frozen_failure = scenario.object_member("failure");
    REQUIRE(frozen_failure != nullptr);
    const auto* frozen_failure_message = support::json_member(*frozen_failure, "message");
    REQUIRE(frozen_failure_message != nullptr);
    const std::string expected_poison = frozen_failure_message->get<std::string>();
    CHECK(expected_poison == "JSONL storage is poisoned and must be reopened");

    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);
    const ConversationId conversation_id = create_conversation(*session);
    const std::string main_before = workspace.read("main.jsonl");

    // Inject the failure at the physical main-marker path: the append targets
    // a directory, so the real append fails after the batch was validated.
    const auto main_path = workspace.path() / "main.jsonl";
    REQUIRE(std::filesystem::remove(main_path));
    REQUIRE(std::filesystem::create_directory(main_path));

    auto failing = run_commit(*session, [&conversation_id](Transaction& transaction) -> support::Expected<void> {
        auto entry = transaction.append_entry(conversation_id, "audit1054.note", chord::Json{1.0});
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        return {};
    });
    REQUIRE_FALSE(failing.has_value());
    CHECK(failing.error().message == expected_poison);

    const auto* frozen_read = scenario.object_member("readAfterFailure");
    REQUIRE(frozen_read != nullptr);
    const auto* frozen_read_message = support::json_member(*frozen_read, "message");
    REQUIRE(frozen_read_message != nullptr);
    const std::string expected_read = frozen_read_message->get<std::string>();
    CHECK(expected_read == expected_poison);

    auto read_after_failure = storage->conversation(conversation_id);
    REQUIRE_FALSE(read_after_failure.has_value());
    CHECK(read_after_failure.error().message == expected_read);

    // The original instance cannot write again, even through a fresh public
    // Storage call that never passes through the failed Session.
    const StorageWrite forbidden{.type = "entry",
            .entry = EntryRecord{.id = EntryId{.value = 4},
                    .conversation_id = conversation_id,
                    .kind = "audit1054.note",
                    .data = chord::Json{2.0}}};
    const std::vector<StorageWrite> forbidden_batch{forbidden};
    const Outcome<Seq> forbidden_outcome = consume(storage->commit(forbidden_batch));
    REQUIRE_FALSE(forbidden_outcome.has_value());
    CHECK(forbidden_outcome.error().message == expected_poison);

    const auto* frozen_end = scenario.object_member("sessionEnd");
    REQUIRE(frozen_end != nullptr);
    const auto* frozen_reason = support::json_member(*frozen_end, "reason");
    const auto* frozen_error = support::json_member(*frozen_end, "errorMessage");
    REQUIRE(frozen_reason != nullptr);
    REQUIRE(frozen_error != nullptr);
    CHECK(frozen_reason->get<std::string>() == "failed");
    CHECK(frozen_error->get<std::string>() == expected_poison);
    CHECK(session->failed());
    CHECK(session->closed().reason == SessionEndReason::Failed);
    REQUIRE(session->closed().error_message.has_value());
    CHECK(*session->closed().error_message == expected_poison);

    // Restore the physical path and reopen: only the published prefix returns.
    REQUIRE(std::filesystem::remove_all(main_path) == 1);
    workspace.write("main.jsonl", main_before);
    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);
    const auto* frozen_entries = scenario.array_member("entriesAfterRecovery");
    REQUIRE(frozen_entries != nullptr);
    std::size_t recovered_entries = 0;
    REQUIRE(run_commit(
            *reopened, [&conversation_id, &recovered_entries](Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.conversation(conversation_id);
                if (!conversation) {
                    return std::unexpected(std::move(conversation.error()));
                }
                CHECK((*conversation).has_value());
                auto entries = transaction.scan_entries(conversation_id, 10);
                if (!entries) {
                    return std::unexpected(std::move(entries.error()));
                }
                recovered_entries = entries->size();
                return {};
            }).has_value());
    CHECK(recovered_entries == frozen_entries->size());
    CHECK(recovered_entries == 0);
}

// Frozen obligation: "a sidecar record is confirmed only by a later marker that
// names it" and "the marker must be the last write".
//
// Discriminating case: injecting the main-marker failure after the sidecar was
// written leaves a complete but unconfirmed sidecar record. A backend that
// writes the marker first leaves no sidecar at all here; a backend that adopts
// an unconfirmed tail resurrects the document on reopen.
TEST_CASE("a JSONL marker failure leaves the sidecar unconfirmed and reopen discards it",
        "[durable][jsonl][poison][sidecar][ordering][issue1074][spec]") {
    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);
    const ConversationId conversation_id = create_conversation(*session);
    const std::string main_before = workspace.read("main.jsonl");

    const auto main_path = workspace.path() / "main.jsonl";
    REQUIRE(std::filesystem::remove(main_path));
    REQUIRE(std::filesystem::create_directory(main_path));

    const auto definition = latest_document();
    auto failing =
            run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                if (!draft) {
                    return std::unexpected(std::move(draft.error()));
                }
                return draft->set(member_path("text"), chord::Json{std::string{"unconfirmed"}});
            });
    REQUIRE_FALSE(failing.has_value());
    CHECK(failing.error().message == "JSONL storage is poisoned and must be reopened");

    // The sidecar was appended before the marker failed: this is the ordering
    // the recovery evidence relies on.
    REQUIRE(std::filesystem::exists(workspace.path() / "doc-3.jsonl"));
    const auto unconfirmed_lines = json_lines(workspace.read("doc-3.jsonl"));
    REQUIRE(unconfirmed_lines.size() == 1);
    const auto* unconfirmed_object = unconfirmed_lines.front().get_if<support::JsonValue::object_t>();
    REQUIRE(unconfirmed_object != nullptr);
    const auto unconfirmed_seq = support::json_integer_member(*unconfirmed_object, "seq");
    REQUIRE(unconfirmed_seq.has_value());
    CHECK(*unconfirmed_seq == 2);

    REQUIRE(std::filesystem::remove_all(main_path) == 1);
    workspace.write("main.jsonl", main_before);
    auto reopened_storage = JsonlStorage::open(workspace.path());
    REQUIRE(reopened_storage.has_value());
    auto reopened = Session::create(**reopened_storage);
    auto snapshot = consume(reopened->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot.value().has_value());
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "doc-3.jsonl"));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "a JSONL checkpoint replaces a current-only sidecar and retirement removes it".
//
// Discriminating case: after the checkpoint the sidecar must contain only the
// new base at sequence 4, not the create base plus the delta; after retirement
// the sidecar must be gone. A backend that appends the checkpoint beside the
// old records, or leaves an empty file behind, fails the file-set comparison.
TEST_CASE("a JSONL checkpoint replaces a current-only sidecar and retirement removes it",
        "[durable][jsonl][checkpoint][retire][issue1074][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kCheckpointScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto* after_first_base = scenario.array_member("afterFirstBase");
    const auto* after_delta = scenario.array_member("afterDelta");
    const auto* after_checkpoint = scenario.array_member("afterCheckpoint");
    const auto* after_retire = scenario.array_member("afterRetire");
    REQUIRE(after_first_base != nullptr);
    REQUIRE(after_delta != nullptr);
    REQUIRE(after_checkpoint != nullptr);
    REQUIRE(after_retire != nullptr);

    TempWorkspace workspace;
    auto opened = JsonlStorage::open(workspace.path());
    REQUIRE(opened.has_value());
    std::unique_ptr<JsonlStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);
    const ConversationId conversation_id = create_conversation(*session);
    const auto definition = latest_document();
    REQUIRE(run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"one"}});
    }).has_value());
    require_files_match(workspace, *after_first_base);

    REQUIRE(run_commit(*session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        return draft->set(member_path("text"), chord::Json{std::string{"two"}});
    }).has_value());
    require_files_match(workspace, *after_delta);

    // A checkpoint is a document.change whose committed content is a base
    // snapshot rather than an operation batch. It replaces the current-only
    // sidecar instead of appending beside the create base and delta.
    //
    // Session in this slice does not expose checkpointWhen or retire_doc (they
    // belong to the later durable document tasks), so Storage::commit is the
    // highest public caller available for these two operations.
    const StorageWrite checkpoint{.type = "document.change",
            .document_id = DocumentId{.value = 3},
            .document_value = chord::Json{chord::Json::Object{
                    {"text", chord::Json{std::string{"checkpoint"}}}, {"revision", chord::Json{0}}}}};
    const std::vector<StorageWrite> checkpoint_batch{checkpoint};
    REQUIRE(consume(storage->commit(checkpoint_batch)).has_value());
    require_files_match(workspace, *after_checkpoint);

    auto checkpoint_storage = JsonlStorage::open(workspace.path());
    REQUIRE(checkpoint_storage.has_value());
    auto checkpoint_session = Session::create(**checkpoint_storage);
    auto snapshot =
            consume(checkpoint_session->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(*snapshot.value() == chord::Json{chord::Json::Object{{"text", chord::Json{std::string{"checkpoint"}}},
                                       {"revision", chord::Json{0}}}});

    const StorageWrite retire{.type = "document.retire", .document_id = DocumentId{.value = 3}};
    const std::vector<StorageWrite> retire_batch{retire};
    REQUIRE(consume(storage->commit(retire_batch)).has_value());
    require_files_match(workspace, *after_retire);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "doc-3.jsonl"));

    auto retired_storage = JsonlStorage::open(workspace.path());
    REQUIRE(retired_storage.has_value());
    auto retired_session = Session::create(**retired_storage);
    auto retired = consume(retired_session->snapshot(definition, DocumentAddress{.conversation_id = conversation_id}));
    REQUIRE(retired.has_value());
    CHECK_FALSE(retired.value().has_value());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/jsonl-store.json
// "JSONL fsync flushes sidecar bytes before the main marker is appended".
//
// Discriminating case: the same document commit runs with the option disabled
// and enabled. Only the enabled trace flushes, and the flush sits between the
// sidecar append and the main marker append. An implementation that always
// flushes fails the disabled trace; one that never flushes, or flushes after
// the marker, fails the enabled trace.
TEST_CASE("JSONL fsync flushes sidecar bytes before the main marker is appended",
        "[durable][jsonl][fsync][issue1174][spec]") {
    const auto loaded = FrozenScenario::load(kJsonlArtifact, kFsyncScenario);
    REQUIRE(loaded.has_value());
    const FrozenScenario& scenario = *loaded;
    const auto* frozen_inputs = scenario.inputs().get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_inputs != nullptr);
    const auto* frozen_options = support::json_object_member(*frozen_inputs, "options");
    REQUIRE(frozen_options != nullptr);
    const auto* frozen_fsync = support::json_member(*frozen_options, "fsync");
    REQUIRE(frozen_fsync != nullptr);
    REQUIRE(frozen_fsync->holds<bool>());
    CHECK(frozen_fsync->get<bool>());
    const auto* frozen_expected = scenario.expected().get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_expected != nullptr);
    const std::vector<std::string> expected_appends = frozen_strings(*frozen_expected, "appendOrder");
    const std::vector<std::string> expected_flushes = frozen_strings(*frozen_expected, "flushOrder");
    CHECK(expected_appends == (std::vector<std::string>{"doc-3.jsonl", "main.jsonl"}));
    CHECK(expected_flushes == (std::vector<std::string>{"doc-3.jsonl"}));
    const auto* frozen_files = support::json_array_member(*frozen_expected, "files");
    REQUIRE(frozen_files != nullptr);

    // The frozen trace is a list of physical operations: every sidecar append,
    // then the flushes in append order, then the main marker append. Building
    // it from the frozen arrays keeps both sections on the registered contract
    // instead of on a hand-written spelling.
    const auto expected_operations = [&](bool fsync) {
        std::vector<std::string> operations;
        operations.reserve(expected_appends.size() + expected_flushes.size());
        for (std::size_t index = 0; index + 1 < expected_appends.size(); ++index) {
            operations.push_back("append:" + expected_appends[index]);
        }
        if (fsync) {
            for (const std::string& file : expected_flushes) {
                operations.push_back("flush:" + file);
            }
        }
        operations.push_back("append:" + expected_appends.back());
        return operations;
    };

    const auto commit_fsynced_document = [](JsonlStorage& storage) {
        auto session = Session::create(storage);
        const ConversationId conversation_id = create_conversation(*session);
        CHECK(conversation_id.value == 2);
        // The frozen capture resets its recording after the conversation
        // commit, so the observed trace covers only the document commit.
        clear_io_trace();
        const auto definition = latest_document();
        auto commit = run_commit(
                *session, [&conversation_id, &definition](Transaction& transaction) -> support::Expected<void> {
                    auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation_id});
                    if (!draft) {
                        return std::unexpected(std::move(draft.error()));
                    }
                    return draft->set(member_path("text"), chord::Json{std::string{"fsynced"}});
                });
        REQUIRE(commit.has_value());
    };

    SECTION("fsync disabled") {
        TempWorkspace workspace;
        start_io_trace();
        auto opened = JsonlStorage::open(workspace.path());
        REQUIRE(opened.has_value());
        commit_fsynced_document(**opened);
        // The option defaults to false: the frozen append order holds and no
        // flush runs at all.
        CHECK(jsonl_io_log().operations == expected_operations(false));
        require_files_match(workspace, *frozen_files);
        stop_io_trace();
    }

    SECTION("fsync enabled") {
        TempWorkspace workspace;
        start_io_trace();
        auto opened = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
        REQUIRE(opened.has_value());
        commit_fsynced_document(**opened);
        CHECK(jsonl_io_log().operations == expected_operations(true));
        require_files_match(workspace, *frozen_files);
        stop_io_trace();
    }
}

// Frozen evidence: packages/durable/test/jsonl-storage.test.ts
// "orders publication flushes exactly and flushes main only to authorize
// reclamation". The registered audit-1054 observation pins only the single
// document commit above; this trace extends the same contract over the four
// publication shapes the frozen test separates.
//
// Recorded divergence, not fixed in this ticket: frozen pi appends sidecars in
// write order, while the C++ encoder emits them in sorted filename order
// (`EncodedCommit::sidecars`). The trace creates documents in ascending ID
// order so the two orders coincide; changing the encoder is out of scope here.
//
// Discriminating case: a flush emitted after the marker, or one emitted for a
// commit that reclaims nothing, fails the entry-only and terminal-task traces.
TEST_CASE("JSONL publication flushes exactly and flushes main only to authorize reclamation",
        "[durable][jsonl][fsync][issue1174]") {
    for (const bool fsync : {false, true}) {
        TempWorkspace workspace;
        start_io_trace();
        auto opened = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = fsync});
        REQUIRE(opened.has_value());
        auto session = Session::create(**opened);
        const ConversationId conversation_id = create_conversation(*session);
        const auto first_id = (*opened)->mint_id();
        const auto second_id = (*opened)->mint_id();
        REQUIRE(first_id.has_value());
        REQUIRE(second_id.has_value());
        const std::string first = "doc-" + std::to_string(*first_id) + ".jsonl";
        const std::string second = "doc-" + std::to_string(*second_id) + ".jsonl";

        // Two session-scoped document creates: every sidecar is appended in
        // order, then flushed in that same order, then the marker is appended.
        clear_io_trace();
        const auto create_document = [](std::int64_t id, std::string kind) {
            return StorageWrite{.type = "document.create",
                    .document_record = DocumentRecord{.id = DocumentId{.value = id}, .kind = std::move(kind)},
                    .document_value = chord::Json{chord::Json::Object{}}};
        };
        const std::vector<StorageWrite> creates{
                create_document(*first_id, "audit1174.first"), create_document(*second_id, "audit1174.second")};
        REQUIRE(consume((*opened)->commit(creates)).has_value());
        const std::vector<std::string> expected_creates = [&] {
            std::vector<std::string> operations{"append:" + first, "append:" + second};
            if (fsync) {
                operations.push_back("flush:" + first);
                operations.push_back("flush:" + second);
            }
            operations.push_back("append:main.jsonl");
            return operations;
        }();
        CHECK(jsonl_io_log().operations == expected_creates);

        // A checkpoint base on a current-only document reclaims the earlier
        // revisions: main is flushed to authorize the replacement, the scratch
        // file is written and flushed, then renamed over the sidecar.
        clear_io_trace();
        const StorageWrite checkpoint{.type = "document.change",
                .document_id = DocumentId{.value = *first_id},
                .document_value = chord::Json{chord::Json::Object{{"checkpoint", chord::Json{true}}}}};
        const std::vector<StorageWrite> checkpoint_batch{checkpoint};
        REQUIRE(consume((*opened)->commit(checkpoint_batch)).has_value());
        const std::vector<std::string> expected_checkpoint = [&] {
            std::vector<std::string> operations{"append:" + first};
            if (fsync) {
                operations.push_back("flush:" + first);
            }
            operations.push_back("append:main.jsonl");
            if (fsync) {
                operations.push_back("flush:main.jsonl");
            }
            operations.push_back("write:" + first + ".reclaim");
            if (fsync) {
                operations.push_back("flush:" + first + ".reclaim");
            }
            operations.push_back("rename:" + first + ".reclaim->" + first);
            return operations;
        }();
        CHECK(jsonl_io_log().operations == expected_checkpoint);

        // An entry-only commit publishes through the marker alone: no sidecar
        // exists to flush and nothing is reclaimed, so fsync adds no flush.
        clear_io_trace();
        const auto entry_id = (*opened)->mint_id();
        REQUIRE(entry_id.has_value());
        const StorageWrite entry{.type = "entry",
                .entry = EntryRecord{.id = EntryId{.value = *entry_id},
                        .conversation_id = conversation_id,
                        .kind = "audit1174.main-only",
                        .data = chord::Json{chord::Json::Object{}}}};
        const std::vector<StorageWrite> entry_batch{entry};
        REQUIRE(consume((*opened)->commit(entry_batch)).has_value());
        CHECK(jsonl_io_log().operations == (std::vector<std::string>{"append:main.jsonl"}));

        // A terminal task reclaims its sidecar: the marker is appended, main is
        // flushed to authorize the removal, then the sidecar is removed.
        const auto task_id = (*opened)->mint_id();
        REQUIRE(task_id.has_value());
        TaskRecord task{.id = TaskId{.value = *task_id},
                .kind = "audit1174.work",
                .version = 1,
                .state = TaskState{.status = "pending",
                        .checkpoint = chord::Json{chord::Json::Object{{"phase", chord::Json{"start"}}}}}};
        const std::vector<StorageWrite> pending{StorageWrite{.type = "task", .task = task}};
        REQUIRE(consume((*opened)->commit(pending)).has_value());
        clear_io_trace();
        task.state = TaskState{.status = "terminal", .outcome = TaskOutcome{.status = "completed"}};
        const std::vector<StorageWrite> terminal{StorageWrite{.type = "task", .task = task}};
        REQUIRE(consume((*opened)->commit(terminal)).has_value());
        const std::vector<std::string> expected_terminal = [&] {
            std::vector<std::string> operations{"append:main.jsonl"};
            if (fsync) {
                operations.push_back("flush:main.jsonl");
            }
            operations.push_back("remove:task-" + std::to_string(*task_id) + ".jsonl");
            return operations;
        }();
        CHECK(jsonl_io_log().operations == expected_terminal);

        stop_io_trace();
        REQUIRE((*opened)->close());
    }
}

// Frozen evidence: packages/durable/test/jsonl-storage.test.ts
// "poisons after {before,after} failure at flush {1,2} and never writes a marker".
//
// Discriminating case: every sidecar is appended before the first flush runs, so
// a backend that flushed each sidecar as it appended it would still append the
// marker after a later flush failed. The marker bytes must be unchanged and the
// reopen must show the pre-change value with the unconfirmed tail truncated.
TEST_CASE("JSONL sidecar flush failure poisons before the marker and never publishes",
        "[durable][jsonl][fsync][issue1174]") {
    constexpr std::string_view kPoisonMessage = "JSONL storage is poisoned and must be reopened";
    const chord::Json base_value{chord::Json::Object{{"text", chord::Json{std::string{"before"}}}}};
    const chord::Json changed_value{chord::Json::Object{{"text", chord::Json{std::string{"after"}}}}};

    for (const std::size_t failing_call : {std::size_t{1}, std::size_t{2}}) {
        CAPTURE(failing_call);
        TempWorkspace workspace;
        {
            auto seeded = JsonlStorage::open(workspace.path());
            REQUIRE(seeded.has_value());
            const StorageWrite base{.type = "document.create",
                    .document_record = DocumentRecord{.id = DocumentId{.value = 3}, .kind = "audit1174.before"},
                    .document_value = base_value};
            const std::vector<StorageWrite> base_batch{base};
            REQUIRE(consume((*seeded)->commit(base_batch)).has_value());
            REQUIRE((*seeded)->close());
        }
        const std::string main_before = workspace.read("main.jsonl");
        const std::string sidecar_before = workspace.read("doc-3.jsonl");
        REQUIRE_FALSE(sidecar_before.empty());

        start_io_trace();
        auto opened = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
        REQUIRE(opened.has_value());
        clear_io_trace();
        // Two sidecars take part in the failing commit: the change to doc-3 and
        // the creation of doc-4. Failing either flush must stop before the
        // marker, so neither sidecar's bytes are published.
        fail_io_call(testing::JsonlIoOperation::Flush, failing_call);
        const StorageWrite change{
                .type = "document.change", .document_id = DocumentId{.value = 3}, .document_value = changed_value};
        const StorageWrite create{.type = "document.create",
                .document_record = DocumentRecord{.id = DocumentId{.value = 4}, .kind = "audit1174.after"},
                .document_value = changed_value};
        const std::vector<StorageWrite> batch{change, create};
        const auto outcome = consume((*opened)->commit(batch));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().message == kPoisonMessage);
        // The flushed sidecars never authorize the marker: main is byte-identical.
        CHECK(workspace.read("main.jsonl") == main_before);
        const auto poisoned_read = (*opened)->document(DocumentId{.value = 3}, DocumentPoint::current());
        REQUIRE_FALSE(poisoned_read.has_value());
        CHECK(poisoned_read.error().message == kPoisonMessage);
        stop_io_trace();
        REQUIRE((*opened)->close());

        auto reopened = JsonlStorage::open(workspace.path());
        REQUIRE(reopened.has_value());
        const auto restored = (*reopened)->document(DocumentId{.value = 3}, DocumentPoint::current());
        REQUIRE(restored.has_value());
        REQUIRE(restored->has_value());
        CHECK((*restored)->value == base_value);
        const auto absent = (*reopened)->document(DocumentId{.value = 4}, DocumentPoint::current());
        REQUIRE(absent.has_value());
        CHECK_FALSE(absent->has_value());
        // The unconfirmed tail is truncated: the pre-existing base line survives
        // and the never-confirmed creation sidecar is removed.
        CHECK(workspace.read("doc-3.jsonl") == sidecar_before);
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "doc-4.jsonl"));
        REQUIRE((*reopened)->close());
    }
}

// Frozen evidence: packages/durable/test/jsonl-storage.test.ts
// "defers reclamation after authorizing-main flush {before,after} failure".
//
// Discriminating case: the main flush is the reclamation gate, not a publication
// step. Failing it must leave the commit published and the store usable, so the
// marker append and the in-memory apply still happen; a backend that treated the
// main flush as part of publication would poison or reject here. Recovery then
// emits the same gate as its only physical operation and defers the pass.
TEST_CASE("JSONL main flush failure defers reclamation without changing the commit",
        "[durable][jsonl][fsync][issue1174]") {
    const chord::Json base_value{chord::Json::Object{{"count", chord::Json{0.0}}}};
    const chord::Json changed_value{chord::Json::Object{{"count", chord::Json{2.0}}}};

    TempWorkspace workspace;
    start_io_trace();
    auto opened = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
    REQUIRE(opened.has_value());
    clear_io_trace();
    const StorageWrite create{.type = "document.create",
            .document_record = DocumentRecord{.id = DocumentId{.value = 3}, .kind = "audit1174.gate"},
            .document_value = base_value};
    const std::vector<StorageWrite> create_batch{create};
    REQUIRE(consume((*opened)->commit(create_batch)).has_value());
    CHECK(jsonl_io_log().operations ==
            (std::vector<std::string>{"append:doc-3.jsonl", "flush:doc-3.jsonl", "append:main.jsonl"}));

    clear_io_trace();
    // Flush 1 is the sidecar and flush 2 is the main gate of the reclamation.
    fail_io_call(testing::JsonlIoOperation::Flush, 2);
    const StorageWrite change{
            .type = "document.change", .document_id = DocumentId{.value = 3}, .document_value = changed_value};
    const std::vector<StorageWrite> change_batch{change};
    const auto committed = consume((*opened)->commit(change_batch));
    REQUIRE(committed.has_value());
    CHECK(committed.value() == 2);
    CHECK(jsonl_io_log().operations ==
            (std::vector<std::string>{
                    "append:doc-3.jsonl", "flush:doc-3.jsonl", "append:main.jsonl", "flush:main.jsonl"}));
    const auto live = (*opened)->document(DocumentId{.value = 3}, DocumentPoint::current());
    REQUIRE(live.has_value());
    REQUIRE(live->has_value());
    CHECK((*live)->value == changed_value);
    CHECK(json_lines(workspace.read("doc-3.jsonl")).size() == 2);
    REQUIRE((*opened)->close());

    // Recovery repeats the gate: the only physical operation is the failed main
    // flush, and the deferred pass leaves both sidecar lines in place.
    clear_io_trace();
    fail_io_call(testing::JsonlIoOperation::Flush, 1);
    auto deferred = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
    REQUIRE(deferred.has_value());
    CHECK(jsonl_io_log().operations == (std::vector<std::string>{"flush:main.jsonl"}));
    const auto deferred_value = (*deferred)->document(DocumentId{.value = 3}, DocumentPoint::current());
    REQUIRE(deferred_value.has_value());
    REQUIRE(deferred_value->has_value());
    CHECK((*deferred_value)->value == changed_value);
    CHECK(json_lines(workspace.read("doc-3.jsonl")).size() == 2);
    REQUIRE((*deferred)->close());

    // A later reopen with a healthy flush applies the deferred pass.
    clear_io_trace();
    auto reclaimed = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
    REQUIRE(reclaimed.has_value());
    const auto reclaimed_value = (*reclaimed)->document(DocumentId{.value = 3}, DocumentPoint::current());
    REQUIRE(reclaimed_value.has_value());
    REQUIRE(reclaimed_value->has_value());
    CHECK((*reclaimed_value)->value == changed_value);
    CHECK(json_lines(workspace.read("doc-3.jsonl")).size() == 1);
    stop_io_trace();
    REQUIRE((*reclaimed)->close());
}

// Frozen evidence: packages/durable/test/jsonl-storage.test.ts
// "keeps a committed base usable after reclaim-temp flush {before,after} failure".
//
// Discriminating case: the scratch file is flushed before it is renamed, and a
// failure there must not roll back or poison the commit that already published
// the base. The scratch file stays for recovery to delete, a later delta commit
// still appends, and the reopen keeps the committed base plus the delta.
TEST_CASE("JSONL reclaim-temp flush failure keeps the committed base usable", "[durable][jsonl][fsync][issue1174]") {
    const chord::Json base_value{chord::Json::Object{{"count", chord::Json{0.0}}}};
    const chord::Json checkpoint_value{chord::Json::Object{{"count", chord::Json{2.0}}}};
    const chord::Json delta_value{chord::Json::Object{{"count", chord::Json{3.0}}}};

    TempWorkspace workspace;
    start_io_trace();
    auto opened = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
    REQUIRE(opened.has_value());
    clear_io_trace();
    const StorageWrite create{.type = "document.create",
            .document_record = DocumentRecord{.id = DocumentId{.value = 3}, .kind = "audit1174.scratch"},
            .document_value = base_value};
    const std::vector<StorageWrite> create_batch{create};
    REQUIRE(consume((*opened)->commit(create_batch)).has_value());

    clear_io_trace();
    // Flush 1 is the sidecar, flush 2 the main gate, flush 3 the scratch file.
    fail_io_call(testing::JsonlIoOperation::Flush, 3);
    const StorageWrite checkpoint{
            .type = "document.change", .document_id = DocumentId{.value = 3}, .document_value = checkpoint_value};
    const std::vector<StorageWrite> checkpoint_batch{checkpoint};
    const auto committed = consume((*opened)->commit(checkpoint_batch));
    REQUIRE(committed.has_value());
    CHECK(committed.value() == 2);
    CHECK(jsonl_io_log().operations == (std::vector<std::string>{"append:doc-3.jsonl",
                                               "flush:doc-3.jsonl",
                                               "append:main.jsonl",
                                               "flush:main.jsonl",
                                               "write:doc-3.jsonl.reclaim",
                                               "flush:doc-3.jsonl.reclaim"}));
    // The failed scratch flush defers the replacement and leaves the scratch
    // file for recovery to delete.
    CHECK(std::filesystem::exists(workspace.path() / "doc-3.jsonl.reclaim"));
    const auto live = (*opened)->document(DocumentId{.value = 3}, DocumentPoint::current());
    REQUIRE(live.has_value());
    REQUIRE(live->has_value());
    CHECK((*live)->value == checkpoint_value);

    clear_io_trace();
    const StorageWrite delta{.type = "document.change",
            .document_id = DocumentId{.value = 3},
            .document_value = delta_value,
            .document_ops =
                    chord::OpBatch{chord::SetOp{.path = {chord::path_key("count")}, .value = chord::Json{3.0}}}};
    const std::vector<StorageWrite> delta_batch{delta};
    REQUIRE(consume((*opened)->commit(delta_batch)).has_value());
    CHECK(jsonl_io_log().operations ==
            (std::vector<std::string>{"append:doc-3.jsonl", "flush:doc-3.jsonl", "append:main.jsonl"}));
    REQUIRE((*opened)->close());

    auto reopened = JsonlStorage::open(workspace.path(), JsonlStorageOptions{.fsync = true});
    REQUIRE(reopened.has_value());
    const auto restored = (*reopened)->document(DocumentId{.value = 3}, DocumentPoint::current());
    REQUIRE(restored.has_value());
    REQUIRE(restored->has_value());
    CHECK((*restored)->value == delta_value);
    CHECK(json_lines(workspace.read("doc-3.jsonl")).size() == 2);
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "doc-3.jsonl.reclaim"));
    stop_io_trace();
    REQUIRE((*reopened)->close());
}

// Criterion 4: the option belongs to one open incarnation. It is neither
// global nor persisted, so an fsync-written store continues under the default
// and a default-written store continues under fsync. No power-loss, SIGKILL or
// multi-process guarantee is added by either direction; only flush ordering is.
TEST_CASE("JSONL fsync is a per-instance option", "[durable][jsonl][fsync][issue1174]") {
    const chord::Json base_value{chord::Json::Object{{"count", chord::Json{0.0}}}};
    const chord::Json checkpoint_value{chord::Json::Object{{"count", chord::Json{1.0}}}};
    const chord::Json next_value{chord::Json::Object{{"count", chord::Json{2.0}}}};
    const auto create_document = [](const chord::Json& value) {
        return StorageWrite{.type = "document.create",
                .document_record = DocumentRecord{.id = DocumentId{.value = 3}, .kind = "audit1174.option"},
                .document_value = value};
    };
    const auto checkpoint_document = [](const chord::Json& value) {
        return StorageWrite{.type = "document.change", .document_id = DocumentId{.value = 3}, .document_value = value};
    };
    const std::vector<std::string> fsynced_checkpoint{"append:doc-3.jsonl",
            "flush:doc-3.jsonl",
            "append:main.jsonl",
            "flush:main.jsonl",
            "write:doc-3.jsonl.reclaim",
            "flush:doc-3.jsonl.reclaim",
            "rename:doc-3.jsonl.reclaim->doc-3.jsonl"};
    const std::vector<std::string> plain_checkpoint{"append:doc-3.jsonl",
            "append:main.jsonl",
            "write:doc-3.jsonl.reclaim",
            "rename:doc-3.jsonl.reclaim->doc-3.jsonl"};

    SECTION("two open instances keep their own option") {
        TempWorkspace synced;
        TempWorkspace plain;
        start_io_trace();
        auto synced_storage = JsonlStorage::open(synced.path(), JsonlStorageOptions{.fsync = true});
        auto plain_storage = JsonlStorage::open(plain.path());
        REQUIRE(synced_storage.has_value());
        REQUIRE(plain_storage.has_value());

        // Interleaved commits: a global flag would leak into the other instance
        // in one direction or the other.
        clear_io_trace();
        const std::vector<StorageWrite> synced_create{create_document(base_value)};
        REQUIRE(consume((*synced_storage)->commit(synced_create)).has_value());
        CHECK(jsonl_io_log().operations ==
                (std::vector<std::string>{"append:doc-3.jsonl", "flush:doc-3.jsonl", "append:main.jsonl"}));

        clear_io_trace();
        const std::vector<StorageWrite> plain_create{create_document(base_value)};
        REQUIRE(consume((*plain_storage)->commit(plain_create)).has_value());
        CHECK(jsonl_io_log().operations == (std::vector<std::string>{"append:doc-3.jsonl", "append:main.jsonl"}));

        clear_io_trace();
        const std::vector<StorageWrite> synced_checkpoint_batch{checkpoint_document(checkpoint_value)};
        REQUIRE(consume((*synced_storage)->commit(synced_checkpoint_batch)).has_value());
        CHECK(jsonl_io_log().operations == fsynced_checkpoint);

        clear_io_trace();
        const std::vector<StorageWrite> plain_checkpoint_batch{checkpoint_document(checkpoint_value)};
        REQUIRE(consume((*plain_storage)->commit(plain_checkpoint_batch)).has_value());
        CHECK(jsonl_io_log().operations == plain_checkpoint);

        stop_io_trace();
        REQUIRE((*synced_storage)->close());
        REQUIRE((*plain_storage)->close());
    }

    // One direction of the reopen: the first incarnation writes under
    // `first_options`, and the second incarnation continues under
    // `second_options` with the trace those options must produce.
    const auto continue_under_option = [&](JsonlStorageOptions first_options,
                                               JsonlStorageOptions second_options,
                                               const std::vector<std::string>& expected_trace) {
        TempWorkspace workspace;
        {
            auto first = JsonlStorage::open(workspace.path(), first_options);
            REQUIRE(first.has_value());
            const std::vector<StorageWrite> create{create_document(base_value)};
            REQUIRE(consume((*first)->commit(create)).has_value());
            const std::vector<StorageWrite> checkpoint{checkpoint_document(checkpoint_value)};
            REQUIRE(consume((*first)->commit(checkpoint)).has_value());
            REQUIRE((*first)->close());
        }

        start_io_trace();
        auto second = JsonlStorage::open(workspace.path(), second_options);
        REQUIRE(second.has_value());
        clear_io_trace();
        const std::vector<StorageWrite> checkpoint{checkpoint_document(next_value)};
        REQUIRE(consume((*second)->commit(checkpoint)).has_value());
        // A leaked or persisted fsync flag would emit the other trace here.
        CHECK(jsonl_io_log().operations == expected_trace);
        stop_io_trace();
        REQUIRE((*second)->close());

        auto reopened = JsonlStorage::open(workspace.path());
        REQUIRE(reopened.has_value());
        const auto stored = (*reopened)->document(DocumentId{.value = 3}, DocumentPoint::current());
        REQUIRE(stored.has_value());
        REQUIRE(stored->has_value());
        CHECK((*stored)->value == next_value);
        REQUIRE((*reopened)->close());
    };

    SECTION("an fsync-written store continues under the default") {
        continue_under_option(JsonlStorageOptions{.fsync = true}, JsonlStorageOptions{}, plain_checkpoint);
    }

    SECTION("a default-written store continues under fsync") {
        continue_under_option(JsonlStorageOptions{}, JsonlStorageOptions{.fsync = true}, fsynced_checkpoint);
    }
}
