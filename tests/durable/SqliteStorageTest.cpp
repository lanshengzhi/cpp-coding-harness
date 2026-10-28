#include <cch/durable/Harness.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests;
using namespace cch::tests::durable;

namespace {

[[nodiscard]] sqlite3* open_database(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    return database;
}

[[nodiscard]] std::string pragma_text(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const std::string result = value == nullptr ? std::string{} : std::string{value};
    sqlite3_finalize(statement);
    return result;
}

[[nodiscard]] std::vector<std::string> query_table_names(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database,
                    "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name",
                    -1,
                    &statement,
                    nullptr) == SQLITE_OK);
    std::vector<std::string> names;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        if (text != nullptr) names.emplace_back(text);
    }
    sqlite3_finalize(statement);
    return names;
}

[[nodiscard]] Outcome<void> run_commit(
        Session& session, std::move_only_function<support::Expected<void>(Transaction&)> change) {
    return consume(session.commit(std::move(change)));
}

[[nodiscard]] DocumentDefinition versioned_document() {
    return define_document("audit1075.session-doc", std::move_only_function<chord::Json()>{[] {
        return chord::Json{chord::Json::Object{{"revision", chord::Json{0.0}}}};
    }});
}

constexpr std::string_view kIssue1086Artifact = "fixtures/pi-v1.3.0/audit-1054/interchange.json";
constexpr std::string_view kIssue1086Scenario =
        "sqlite store created by pi reopens with unfinished task, documents and checkpoints and continues";
// Every indexed string carries a quote, a backslash and a non-ASCII code point
// so the SQLite columns exercise frozen pi's `JSON.stringify` encoding rather
// than a plain quoted or raw spelling.
constexpr std::string_view kIssue1086TaskKind = "audit1086.work\"\\é";
constexpr std::string_view kIssue1086DocumentKind = "audit1086.history\"\\é";
constexpr std::string_view kIssue1086RequestId = "audit1086/request\"\\é";

[[nodiscard]] chord::Json start_checkpoint() { return chord::Json{chord::Json::Object{{"phase", "start"}, {"n", 7}}}; }

[[nodiscard]] chord::Json finish_checkpoint() {
    return chord::Json{chord::Json::Object{{"phase", "finish"}, {"n", 8}}};
}

[[nodiscard]] chord::Path member_path(std::string key) { return chord::Path{chord::path_key(std::move(key))}; }

[[nodiscard]] support::Expected<DocumentDefinition> continuation_document() {
    return define_document(DocumentDefinitionBuilder{std::string{kIssue1086DocumentKind}}.version(1).conversation(
                                   DocumentHistory::Rewindable, DocumentFork::AsOf),
            [] { return chord::Json{chord::Json::Object{{"n", chord::Json{0}}}}; });
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void execute_sql(const std::filesystem::path& path, std::string_view sql) {
    sqlite3* database = open_database(path);
    char* error = nullptr;
    const int result = sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &error);
    const std::string detail = error == nullptr ? std::string{} : std::string{error};
    if (error != nullptr) sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK) INFO(detail);
    REQUIRE(result == SQLITE_OK);
}

[[nodiscard]] std::filesystem::path extract_pi_sqlite_fixture(const std::filesystem::path& directory) {
    const auto path = directory / "pi-storage.sqlite";
    REQUIRE(directory.string().find_first_of("'\n") == std::string::npos);
    const std::string command =
            "python3 '" CCH_SOURCE_DIR "/tests/durable/extract_sqlite_fixture.py' '" CCH_SOURCE_DIR "/" +
            std::string{kIssue1086Artifact} + "' '" + path.string() + "' '" + std::string{kIssue1086Scenario} + "'";
    REQUIRE(std::system(command.c_str()) == 0);
    return path;
}

struct PiSqliteIds {
    TaskId task{};
    SubmissionId submission{};
    ConversationId conversation{};
};

[[nodiscard]] PiSqliteIds read_pi_sqlite_ids(const std::filesystem::path& directory) {
    auto parsed = support::read_json(read_text_file(directory / "pi-created.json"));
    REQUIRE(parsed.has_value());
    const auto* object = parsed->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto number = [&](std::string_view key) {
        const auto found = object->find(std::string{key});
        REQUIRE(found != object->end());
        const auto* value = found->second.get_if<double>();
        REQUIRE(value != nullptr);
        return static_cast<std::int64_t>(*value);
    };
    return PiSqliteIds{.task = TaskId{number("id")},
            .submission = SubmissionId{number("submission")},
            .conversation = ConversationId{number("conversation")}};
}

[[nodiscard]] std::string pi_sqlite_command(const std::filesystem::path& directory,
        std::string_view mode,
        std::optional<TaskId> task = std::nullopt,
        std::optional<SubmissionId> submission = std::nullopt,
        std::optional<ConversationId> conversation = std::nullopt) {
    const char* checkout = std::getenv("PI_CHECKOUT");
    REQUIRE(checkout != nullptr);
    const std::filesystem::path root{checkout};
    REQUIRE(root.string().find_first_of("'\n") == std::string::npos);
    REQUIRE(directory.string().find_first_of("'\n") == std::string::npos);
    std::string command = "timeout 30 env NODE_OPTIONS=--conditions=source TSX_TSCONFIG_PATH='" +
                          (root / "tsconfig.json").string() + "' '" + (root / "node_modules/.bin/tsx").string() +
                          "' '" CCH_SOURCE_DIR "/fixtures/pi-v1.3.0/audit-1086/continue-1086-sqlite.mts' '" +
                          directory.string() + "' " + std::string{mode};
    if (task.has_value() || submission.has_value() || conversation.has_value()) {
        REQUIRE(task.has_value());
        REQUIRE(submission.has_value());
        REQUIRE(conversation.has_value());
        command += " " + std::to_string(task->value) + " " + std::to_string(submission->value) + " " +
                   std::to_string(conversation->value);
    }
    return command;
}

void run_pi_sqlite(const std::filesystem::path& directory,
        std::string_view mode,
        std::optional<TaskId> task = std::nullopt,
        std::optional<SubmissionId> submission = std::nullopt,
        std::optional<ConversationId> conversation = std::nullopt) {
    REQUIRE(std::system(pi_sqlite_command(directory, mode, task, submission, conversation).c_str()) == 0);
}

[[nodiscard]] std::vector<std::int64_t> document_revision_seqs(const std::filesystem::path& path, DocumentId id) {
    sqlite3* database = open_database(path);
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database,
                    "SELECT seq FROM document_revisions WHERE document_id=?1 ORDER BY seq",
                    -1,
                    &statement,
                    nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_int64(statement, 1, id.value) == SQLITE_OK);
    std::vector<std::int64_t> seqs;
    while (sqlite3_step(statement) == SQLITE_ROW)
        seqs.push_back(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return seqs;
}

[[nodiscard]] support::ExpectedVoid finish_with_cpp(
        SqliteStorage& storage, DocumentDefinition& history, TaskId task_id) {
    TaskDefinitionBuilder builder{std::string{kIssue1086TaskKind}, 1};
    builder.phase("finish", [&history](const TaskRecord& record, TaskRuntime& runtime) -> support::AsyncResult<void> {
        if (!record.state.checkpoint || *record.state.checkpoint != finish_checkpoint()) {
            return support::AsyncResult<void>{std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "unexpected C++ finish checkpoint"))};
        }
        return runtime.commit([&history](Transaction& tx,
                                      const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
            auto draft = tx.doc(history, DocumentAddress{.conversation_id = current.conversation_id});
            if (!draft) return std::unexpected(draft.error());
            if (auto changed = draft->set(member_path("n"), chord::Json{3}); !changed)
                return std::unexpected(changed.error());
            return NextTaskState::terminal(TaskOutcome{.status = "completed", .result = chord::Json{17}});
        });
    });
    auto definition = builder.build();
    if (!definition) return std::unexpected(definition.error());
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*definition));
    auto opened = consume(Harness::open(storage, HarnessOptions{.tasks = std::move(tasks)}));
    if (!opened.has_value()) return std::unexpected(opened.error());
    auto harness = std::move(opened.value());
    auto opened_tasks = consume(harness->open_tasks());
    if (!opened_tasks.has_value()) return std::unexpected(opened_tasks.error());
    if (!harness->resume()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "C++ Harness resume was rejected"));
    }
    auto receipt = consume(harness->wait_for_task(task_id));
    auto closed = consume(harness->close());
    if (!receipt.has_value()) return std::unexpected(receipt.error());
    if (!closed.has_value()) return std::unexpected(closed.error());
    if (receipt.value().state.status != "terminal" || !receipt.value().state.outcome ||
            receipt.value().state.outcome->status != "completed" ||
            receipt.value().state.outcome->result != chord::Json{17}) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "unexpected C++ terminal receipt"));
    }
    return {};
}

} // namespace

TEST_CASE("SQLite storage matches frozen audit-1054 baseline schema tables and schema version",
        "[durable][sqlite][schema][frozen][issue1075][spec]") {
    const auto frozen = FrozenScenario::load("fixtures/pi-v1.3.0/audit-1054/sqlite-store.json",
            "the SQLite schema records the durable tables, metadata and document revisions");
    REQUIRE(frozen.has_value());

    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    REQUIRE((*opened)->close().has_value());

    sqlite3* database = open_database(path);
    const auto tables = query_table_names(database);
    sqlite3_close(database);

    const auto* rows = frozen->object_member("rows");
    REQUIRE(rows != nullptr);
    const auto tables_found = rows->find("tables");
    REQUIRE(tables_found != rows->end());
    const auto* frozen_tables = tables_found->second.get_if<support::JsonValue::array_t>();
    REQUIRE(frozen_tables != nullptr);

    std::vector<std::string> expected_tables;
    for (const auto& item : *frozen_tables) {
        const auto* obj = item.get_if<support::JsonValue::object_t>();
        REQUIRE(obj != nullptr);
        const auto name_found = obj->find("name");
        REQUIRE(name_found != obj->end());
        const auto* name = name_found->second.get_if<std::string>();
        REQUIRE(name != nullptr);
        expected_tables.push_back(*name);
    }

    CHECK(tables == expected_tables);
}

TEST_CASE("SQLite rolls back mid-batch failures and keeps the sequence unchanged",
        "[durable][sqlite][rollback][sequence][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);

    const std::array first_batch{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 2}}}};
    const Outcome<Seq> first = consume(storage->commit(first_batch));
    REQUIRE(first.has_value());
    CHECK(first.value() == 1);

    const std::array failing_batch{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 3}}},
            StorageWrite{.type = "document.create",
                    .document_record = DocumentRecord{.id = DocumentId{.value = 4}, .kind = "audit1075.same"},
                    .document_value = chord::Json{chord::Json::Object{{"revision", chord::Json{1}}}}},
            StorageWrite{.type = "document.create",
                    .document_record = DocumentRecord{.id = DocumentId{.value = 5}, .kind = "audit1075.same"},
                    .document_value = chord::Json{chord::Json::Object{{"revision", chord::Json{2}}}}}};
    const Outcome<Seq> failed = consume(storage->commit(failing_batch));
    REQUIRE_FALSE(failed.has_value());

    const auto rolled_back = storage->conversation(ConversationId{.value = 3});
    REQUIRE(rolled_back.has_value());
    CHECK_FALSE(rolled_back->has_value());
    const auto missing_document = storage->find_document("audit1075.same", DocumentAddress{}, DocumentPoint::current());
    REQUIRE(missing_document.has_value());
    CHECK_FALSE(missing_document->has_value());

    const std::array next_batch{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 6}}}};
    const Outcome<Seq> next = consume(storage->commit(next_batch));
    REQUIRE(next.has_value());
    CHECK(next.value() == 2);

    REQUIRE(storage->close().has_value());
    opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    storage = std::move(*opened);
    const std::array reopen_batch{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 7}}}};
    const Outcome<Seq> reopened = consume(storage->commit(reopen_batch));
    REQUIRE(reopened.has_value());
    CHECK(reopened.value() == 3);
}

TEST_CASE("Session rolls back mid-batch trigger failure and keeps sequence unchanged",
        "[durable][sqlite][session][rollback][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);

    sqlite3* database = open_database(path);
    REQUIRE(sqlite3_exec(database,
                    "CREATE TRIGGER fail_test_document BEFORE INSERT ON documents "
                    "BEGIN SELECT RAISE(ABORT, 'injected SQLite document failure'); END",
                    nullptr,
                    nullptr,
                    nullptr) == SQLITE_OK);
    sqlite3_close(database);

    const auto definition = versioned_document();
    const Outcome<void> failed =
            run_commit(*session, [&definition](Transaction& transaction) -> support::Expected<void> {
                auto conversation = transaction.create_conversation();
                if (!conversation) return std::unexpected(std::move(conversation.error()));
                auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = conversation->id});
                if (!draft) return std::unexpected(std::move(draft.error()));
                return {};
            });
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().detail.find("injected SQLite document failure") != std::string::npos);
    CHECK(session->failed());

    auto reopened = SqliteStorage::open(path);
    REQUIRE(reopened.has_value());
    storage = std::move(*reopened);
    const auto conversations = storage->scan_conversations(10);
    REQUIRE(conversations.has_value());
    CHECK(conversations->empty());

    database = open_database(path);
    REQUIRE(sqlite3_exec(database, "DROP TRIGGER fail_test_document", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    auto recovered_session = Session::create(*storage);
    bool committed = false;
    REQUIRE(run_commit(*recovered_session, [&committed](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) return std::unexpected(std::move(conversation.error()));
        committed = true;
        return {};
    }).has_value());
    CHECK(committed);
    REQUIRE(consume(recovered_session->close()).has_value());
}

TEST_CASE("Session executes normal transactions, filtered scans, and document revisions over SQLite",
        "[durable][sqlite][session][normal][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    auto session = Session::create(*storage);

    const auto definition = versioned_document();
    ConversationId cid{};
    REQUIRE(run_commit(*session, [&definition, &cid](Transaction& transaction) -> support::Expected<void> {
        auto conversation = transaction.create_conversation();
        if (!conversation) return std::unexpected(std::move(conversation.error()));
        cid = conversation->id;
        auto entry1 =
                transaction.append_entry(cid, "user", chord::Json{chord::Json::Object{{"text", chord::Json{"hello"}}}});
        if (!entry1) return std::unexpected(std::move(entry1.error()));
        auto entry2 = transaction.append_entry(
                cid, "assistant", chord::Json{chord::Json::Object{{"text", chord::Json{"world"}}}});
        if (!entry2) return std::unexpected(std::move(entry2.error()));
        auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = cid});
        if (!draft) return std::unexpected(std::move(draft.error()));
        return {};
    }).has_value());

    // Update document revision in a second commit.
    REQUIRE(run_commit(*session, [&definition, cid](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{.conversation_id = cid});
        if (!draft) return std::unexpected(std::move(draft.error()));
        auto updated = draft->set(chord::Path{chord::path_key("revision")}, chord::Json{1.0});
        if (!updated) return std::unexpected(std::move(updated.error()));
        return {};
    }).has_value());

    const auto snap = consume(session->snapshot(definition, DocumentAddress{.conversation_id = cid}));
    REQUIRE(snap.has_value());
    REQUIRE(snap.value().has_value());
    REQUIRE(snap.value()->as_object() != nullptr);
    CHECK(snap.value()->as_object()->at("revision") == chord::Json{1.0});

    REQUIRE(consume(session->close()).has_value());

    // Reopen storage and a new session, verify entries and documents persist.
    opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    storage = std::move(*opened);
    auto reopened_session = Session::create(*storage);

    const auto entries = storage->scan_entries(cid, 10);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    CHECK((*entries)[0].kind == "assistant");
    CHECK((*entries)[1].kind == "user");

    const auto reopened_snap = consume(reopened_session->snapshot(definition, DocumentAddress{.conversation_id = cid}));
    REQUIRE(reopened_snap.has_value());
    REQUIRE(reopened_snap.value().has_value());
    REQUIRE(reopened_snap.value()->as_object() != nullptr);
    CHECK(reopened_snap.value()->as_object()->at("revision") == chord::Json{1.0});

    REQUIRE(consume(reopened_session->close()).has_value());
}

TEST_CASE("SQLite supports filtered entry scans, document revisions, and retirement",
        "[durable][sqlite][scans][documents][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);

    const std::array conversations{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 2}}},
            StorageWrite{
                    .type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 10}}}};
    REQUIRE(consume(storage->commit(conversations)).has_value());

    const std::array entries{StorageWrite{.type = "entry",
                                     .entry = EntryRecord{.id = EntryId{.value = 3},
                                             .conversation_id = ConversationId{.value = 2},
                                             .kind = "prompt",
                                             .data = chord::Json{chord::Json::Object{{"value", chord::Json{1}}}}}},
            StorageWrite{.type = "entry",
                    .entry = EntryRecord{.id = EntryId{.value = 4},
                            .conversation_id = ConversationId{.value = 2},
                            .kind = "reply",
                            .data = chord::Json{chord::Json::Object{{"value", chord::Json{2}}}}}},
            StorageWrite{.type = "entry",
                    .entry = EntryRecord{.id = EntryId{.value = 11},
                            .conversation_id = ConversationId{.value = 10},
                            .kind = "other",
                            .data = chord::Json{chord::Json::Object{{"value", chord::Json{3}}}}}}};
    REQUIRE(consume(storage->commit(entries)).has_value());

    const std::array create_document{StorageWrite{.type = "document.create",
            .document_record = DocumentRecord{.id = DocumentId{.value = 12}, .kind = "audit1075.versioned"},
            .document_value = chord::Json{chord::Json::Object{{"revision", chord::Json{1}}}}}};
    const Outcome<Seq> created = consume(storage->commit(create_document));
    REQUIRE(created.has_value());
    CHECK(created.value() == 3);

    const chord::Json updated_value{chord::Json::Object{{"revision", chord::Json{2}}}};
    const std::array change_document{StorageWrite{.type = "document.change",
            .document_id = DocumentId{.value = 12},
            .document_value = updated_value,
            .document_ops = chord::OpBatch{chord::ReplaceOp{.value = updated_value}}}};
    const Outcome<Seq> updated = consume(storage->commit(change_document));
    REQUIRE(updated.has_value());
    CHECK(updated.value() == 4);

    auto filtered = storage->scan_entries(ConversationId{.value = 2}, 10);
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->size() == 2);
    CHECK((*filtered)[0].id == EntryId{.value = 4});
    CHECK((*filtered)[1].id == EntryId{.value = 3});

    REQUIRE(storage->close().has_value());
    opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    storage = std::move(*opened);

    filtered = storage->scan_entries(ConversationId{.value = 2}, 10);
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->size() == 2);
    CHECK((*filtered)[0].id == EntryId{.value = 4});
    CHECK((*filtered)[1].id == EntryId{.value = 3});

    const auto value = storage->document(DocumentId{.value = 12}, DocumentPoint::current());
    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    CHECK((**value).value == chord::Json{chord::Json::Object{{"revision", chord::Json{2}}}});

    const std::array retire_document{StorageWrite{.type = "document.retire", .document_id = DocumentId{.value = 12}}};
    const Outcome<Seq> retired = consume(storage->commit(retire_document));
    if (!retired.has_value()) INFO(retired.error().message << ": " << retired.error().detail);
    REQUIRE(retired.has_value());
    CHECK(retired.value() == 5);

    const auto missing = storage->find_document("audit1075.versioned", DocumentAddress{}, DocumentPoint::current());
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->has_value());

    const std::array next_conversation{StorageWrite{
            .type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 20}}}};
    const Outcome<Seq> continued = consume(storage->commit(next_conversation));
    REQUIRE(continued.has_value());
    CHECK(continued.value() == 6);
}

TEST_CASE("SQLite recovery repairs lagging allocator metadata from stored history",
        "[durable][sqlite][recovery][metadata][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    const auto id = storage->mint_id();
    if (!id) INFO(id.error().message << ": " << id.error().detail);
    REQUIRE(id.has_value());
    CHECK(*id == 2);
    const std::array write{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 20}}},
            StorageWrite{.type = "entry",
                    .entry = EntryRecord{.id = EntryId{.value = 21},
                            .conversation_id = ConversationId{.value = 20},
                            .kind = "audit1075.repair",
                            .data = chord::Json{chord::Json::Object{}}}}};
    const Outcome<Seq> outcome = consume(storage->commit(write));
    REQUIRE(outcome.has_value());
    CHECK(outcome.value() == 1);
    REQUIRE(storage->close().has_value());

    sqlite3* database = open_database(path);
    REQUIRE(sqlite3_exec(database, "UPDATE durable_metadata SET next_id='2',next_seq=1", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
    sqlite3_close(database);

    opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    storage = std::move(*opened);
    const auto next_id = storage->mint_id();
    REQUIRE(next_id.has_value());
    CHECK(*next_id == 22);
    const std::array next_write{StorageWrite{
            .type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 23}}}};
    const Outcome<Seq> next = consume(storage->commit(next_write));
    REQUIRE(next.has_value());
    CHECK(next.value() == 2);
}

TEST_CASE("SQLite rejects record ID ledger corruption during recovery",
        "[durable][sqlite][recovery][corruption][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    const std::array write{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 2}}}};
    REQUIRE(consume((*opened)->commit(write)).has_value());
    REQUIRE((*opened)->close().has_value());

    sqlite3* database = open_database(path);
    REQUIRE(sqlite3_exec(database, "DELETE FROM record_ids WHERE id=2", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);

    auto rejected = SqliteStorage::open(path);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.find("Persisted record ID") != std::string::npos);
}

TEST_CASE("SQLite sequences empty commits and carries their sequence across a reopen",
        "[durable][sqlite][sequence][recovery][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    auto storage = std::move(*opened);
    const std::array<cch::durable::StorageWrite, 0> empty{};
    const Outcome<Seq> empty_outcome = consume(storage->commit(empty));
    REQUIRE(empty_outcome.has_value());
    CHECK(empty_outcome.value() == 1);
    REQUIRE(storage->close().has_value());

    opened = SqliteStorage::open(path);
    REQUIRE(opened.has_value());
    storage = std::move(*opened);
    const std::array write{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 2}}}};
    const Outcome<Seq> write_outcome = consume(storage->commit(write));
    REQUIRE(write_outcome.has_value());
    CHECK(write_outcome.value() == 2);
}

TEST_CASE("SQLite prefixes isolate schemas and reject a newer schema version",
        "[durable][sqlite][prefix][schema][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto left = SqliteStorage::open(path, SqliteStorageOptions{.prefix = "left_"});
    if (!left) FAIL(left.error().message << ": " << left.error().detail);
    const std::array write{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 2}}}};
    REQUIRE(consume((*left)->commit(write)).has_value());

    auto right = SqliteStorage::open(path, SqliteStorageOptions{.prefix = "right_"});
    if (!right) FAIL(right.error().message << ": " << right.error().detail);
    const auto isolated = (*right)->scan_conversations(10);
    REQUIRE(isolated.has_value());
    CHECK(isolated->empty());
    const auto stored = (*left)->scan_conversations(10);
    REQUIRE(stored.has_value());
    REQUIRE(stored->size() == 1);

    REQUIRE((*left)->close().has_value());
    REQUIRE((*right)->close().has_value());
    sqlite3* database = open_database(path);
    REQUIRE(sqlite3_exec(database,
                    "UPDATE left_durable_schema SET version=2 WHERE singleton=1",
                    nullptr,
                    nullptr,
                    nullptr) == SQLITE_OK);
    sqlite3_close(database);

    auto rejected = SqliteStorage::open(path, SqliteStorageOptions{.prefix = "left_"});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.find("newer than supported") != std::string::npos);
}

TEST_CASE("SQLite honors configured busy timeout and WAL checkpoint options",
        "[durable][sqlite][options][busy][issue1075][spec]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened =
            SqliteStorage::open(path, SqliteStorageOptions{.busy_timeout_ms = 40, .wal_auto_checkpoint_pages = 7});
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);

    sqlite3* blocker = open_database(path);
    REQUIRE(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    const std::array write{
            StorageWrite{.type = "conversation", .conversation = ConversationRecord{.id = ConversationId{.value = 2}}}};
    const auto started = std::chrono::steady_clock::now();
    const Outcome<Seq> outcome = consume(storage->commit(write));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Busy);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    (void)elapsed;
#else
    CHECK(elapsed >= std::chrono::milliseconds{30});
#endif
    REQUIRE(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(blocker);

    REQUIRE(storage->close().has_value());
    // journal_mode is persistent and confirms that the configured storage
    // connection opened this database in WAL mode.
    sqlite3* database = open_database(path);
    CHECK(pragma_text(database, "PRAGMA journal_mode") == "wal");
    sqlite3_close(database);
}

TEST_CASE("a pi SQLite store reopens in C++ with unfinished task, documents and checkpoints",
        "[durable][sqlite][issue1086][continuation]") {
    const auto frozen = FrozenScenario::load(kIssue1086Artifact, kIssue1086Scenario);
    REQUIRE(frozen.has_value());
    const auto* phases = frozen->array_member("phases");
    REQUIRE(phases != nullptr);
    REQUIRE(phases->size() >= 1);
    const auto* initial = (*phases)[0].get_if<support::JsonValue::object_t>();
    REQUIRE(initial != nullptr);

    TempWorkspace workspace;
    const auto path = extract_pi_sqlite_fixture(workspace.path());
    sqlite3* raw = open_database(path);
    CHECK(pragma_text(raw, "SELECT version FROM durable_schema WHERE singleton=1") == "1");
    CHECK(pragma_text(raw, "SELECT next_id FROM durable_metadata WHERE singleton=1") == "8");
    CHECK(pragma_text(raw, "SELECT next_seq FROM durable_metadata WHERE singleton=1") == "5");
    sqlite3_close(raw);

    auto opened = SqliteStorage::open(path);
    if (!opened) FAIL(opened.error().message << ": " << opened.error().detail);
    REQUIRE(opened.has_value());
    auto storage = std::move(*opened);

    auto task = storage->task(TaskId{6});
    REQUIRE(task.has_value());
    REQUIRE(task->has_value());
    CHECK((*task)->kind == "audit1054.work");
    CHECK((*task)->state.status == "pending");
    REQUIRE((*task)->state.checkpoint.has_value());
    const auto* expected_task = initial->at("task").get_if<support::JsonValue::object_t>();
    REQUIRE(expected_task != nullptr);
    const auto* expected_state = expected_task->at("state").get_if<support::JsonValue::object_t>();
    REQUIRE(expected_state != nullptr);
    CHECK(json_equals(*(*task)->state.checkpoint, expected_state->at("checkpoint")));
    CHECK(json_equals((*task)->input, expected_task->at("input")));

    const DocumentAddress address{.conversation_id = ConversationId{2}};
    auto latest_record = storage->find_document("audit1054.latest", address, DocumentPoint::current());
    REQUIRE(latest_record.has_value());
    REQUIRE(latest_record->has_value());
    CHECK((*latest_record)->id == DocumentId{4});
    auto latest = storage->document((*latest_record)->id, DocumentPoint::current());
    REQUIRE(latest.has_value());
    REQUIRE(latest->has_value());
    CHECK(json_equals((*latest)->value, initial->at("latest")));

    auto rewind_record = storage->find_document("audit1054.rewind", address, DocumentPoint::current());
    REQUIRE(rewind_record.has_value());
    REQUIRE(rewind_record->has_value());
    CHECK((*rewind_record)->id == DocumentId{5});
    auto rewind = storage->document((*rewind_record)->id, DocumentPoint::current());
    REQUIRE(rewind.has_value());
    REQUIRE(rewind->has_value());
    CHECK(json_equals((*rewind)->value, initial->at("rewind")));
    auto rewind_first = storage->document((*rewind_record)->id, DocumentPoint::as_of(2));
    REQUIRE(rewind_first.has_value());
    REQUIRE(rewind_first->has_value());
    CHECK(json_equals((*rewind_first)->value, initial->at("asOfFirst")));

    auto entries = storage->scan_entries(ConversationId{2}, 10);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    const auto* expected_entries = initial->at("entries").get_if<support::JsonValue::array_t>();
    REQUIRE(expected_entries != nullptr);
    REQUIRE(expected_entries->size() == 2);
    for (std::size_t i = 0; i < entries->size(); ++i) {
        const auto* expected = (*expected_entries)[i].get_if<support::JsonValue::object_t>();
        REQUIRE(expected != nullptr);
        CHECK((*entries)[i].id.value == static_cast<std::int64_t>(expected->at("id").get<double>()));
        CHECK((*entries)[i].kind == expected->at("kind").get<std::string>());
        CHECK(json_equals((*entries)[i].data, expected->at("data")));
    }

    auto documents = storage->scan_conversation_documents(ConversationId{2}, DocumentPoint::current(), 10);
    REQUIRE(documents.has_value());
    REQUIRE(documents->size() == 2);
    CHECK((*documents)[0].id == DocumentId{4});
    CHECK((*documents)[1].id == DocumentId{5});

    auto minted = storage->mint_id();
    REQUIRE(minted.has_value());
    CHECK(*minted == 8);
    REQUIRE(storage->close().has_value());

    const auto malformed_directory = workspace.path() / "malformed";
    REQUIRE(std::filesystem::create_directories(malformed_directory));
    const auto raw_path = extract_pi_sqlite_fixture(malformed_directory);
    execute_sql(raw_path, "UPDATE documents SET kind='audit1054.latest' WHERE id=4");
    auto raw_rejected = SqliteStorage::open(raw_path);
    REQUIRE_FALSE(raw_rejected.has_value());
    CHECK(raw_rejected.error().message.find("Persisted document columns") != std::string::npos);

    const auto non_string_directory = workspace.path() / "non-string";
    REQUIRE(std::filesystem::create_directories(non_string_directory));
    const auto non_string_path = extract_pi_sqlite_fixture(non_string_directory);
    execute_sql(non_string_path, "UPDATE documents SET kind='123' WHERE id=4");
    auto non_string_rejected = SqliteStorage::open(non_string_path);
    REQUIRE_FALSE(non_string_rejected.has_value());
    CHECK(non_string_rejected.error().message.find("Persisted document columns") != std::string::npos);
}

TEST_CASE("pi creates a SQLite store, C++ continues it, and pi finishes it",
        "[durable][sqlite][issue1086][continuation]") {
    TempWorkspace workspace;
    run_pi_sqlite(workspace.path(), "create");
    const PiSqliteIds ids = read_pi_sqlite_ids(workspace.path());

    auto opened = SqliteStorage::open(workspace.path() / "storage.sqlite");
    if (!opened) FAIL(opened.error().message << ": " << opened.error().detail);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    auto history = continuation_document();
    REQUIRE(history.has_value());

    auto initial_task = storage->task(ids.task);
    REQUIRE(initial_task.has_value());
    REQUIRE(initial_task->has_value());
    CHECK((*initial_task)->kind == kIssue1086TaskKind);
    CHECK((*initial_task)->state.status == "pending");
    REQUIRE((*initial_task)->state.checkpoint.has_value());
    CHECK(*(*initial_task)->state.checkpoint == start_checkpoint());
    auto scanned_tasks = storage->scan_tasks(10);
    REQUIRE(scanned_tasks.has_value());
    bool scanned_kind = false;
    for (const TaskRecord& record : *scanned_tasks)
        scanned_kind = scanned_kind || record.kind == kIssue1086TaskKind;
    CHECK(scanned_kind);
    const DocumentAddress address{.conversation_id = ids.conversation};
    auto initial_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(initial_document.has_value());
    REQUIRE(initial_document->has_value());
    auto initial_value = storage->document((*initial_document)->id, DocumentPoint::current());
    REQUIRE(initial_value.has_value());
    REQUIRE(initial_value->has_value());
    CHECK((*initial_value)->value == chord::Json{chord::Json::Object{{"n", chord::Json{1}}}});
    auto initial_submission = storage->submission_by_request(ids.conversation, kIssue1086RequestId);
    REQUIRE(initial_submission.has_value());
    REQUIRE(initial_submission->has_value());
    CHECK((*initial_submission)->id == ids.submission);
    CHECK((*initial_submission)->status == "queued");

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool reached = false;
    bool released = false;
    TaskDefinitionBuilder builder{std::string{kIssue1086TaskKind}, 1};
    builder.phase("start",
            [&history, submission = ids.submission](
                    const TaskRecord& task, TaskRuntime& runtime) -> support::AsyncResult<void> {
                if (!task.state.checkpoint || *task.state.checkpoint != start_checkpoint()) {
                    return support::AsyncResult<void>{std::unexpected(
                            support::make_error(support::ErrorCode::Validation, "unexpected pi start checkpoint"))};
                }
                return runtime.commit(
                        [&history, submission](Transaction& tx,
                                const TaskRecord& current) -> support::Expected<std::optional<NextTaskState>> {
                            auto draft = tx.doc(*history, DocumentAddress{.conversation_id = current.conversation_id});
                            if (!draft) return std::unexpected(draft.error());
                            if (auto changed = draft->set(member_path("n"), chord::Json{2}); !changed)
                                return std::unexpected(changed.error());
                            if (auto settled = tx.settle_submission(submission,
                                        SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                                    !settled)
                                return std::unexpected(settled.error());
                            return NextTaskState::running(finish_checkpoint());
                        });
            });
    builder.phase("finish",
            [&gate_mutex, &gate_cv, &reached, &released](
                    const TaskRecord&, TaskRuntime& runtime) -> support::AsyncResult<void> {
                std::unique_lock lock(gate_mutex);
                reached = true;
                gate_cv.notify_all();
                std::stop_callback wake{runtime.stop_token(), [&] { gate_cv.notify_all(); }};
                if (!gate_cv.wait_for(lock, std::chrono::seconds{10}, [&] {
                        return released || runtime.stop_token().stop_requested();
                    })) {
                    return support::AsyncResult<void>{std::unexpected(
                            support::make_error(support::ErrorCode::Timeout, "C++ finish gate timed out"))};
                }
                return support::AsyncResult<void>{support::ExpectedVoid{}};
            });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*definition));
    auto harness_opened = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(tasks)}));
    if (!harness_opened.has_value()) FAIL(harness_opened.error().message << ": " << harness_opened.error().detail);
    REQUIRE(harness_opened.has_value());
    auto harness = std::move(harness_opened.value());
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
    harness.reset();
    storage.reset();

    run_pi_sqlite(workspace.path(), "finish", ids.task, ids.submission, ids.conversation);

    auto reopened = SqliteStorage::open(workspace.path() / "storage.sqlite");
    if (!reopened) FAIL(reopened.error().message << ": " << reopened.error().detail);
    REQUIRE(reopened.has_value());
    storage = std::move(*reopened);
    auto final_task = storage->task(ids.task);
    REQUIRE(final_task.has_value());
    REQUIRE(final_task->has_value());
    CHECK((*final_task)->state.status == "terminal");
    REQUIRE((*final_task)->state.outcome.has_value());
    CHECK((*final_task)->state.outcome->status == "completed");
    CHECK((*final_task)->state.outcome->result == chord::Json{17});

    auto final_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(final_document.has_value());
    REQUIRE(final_document->has_value());
    auto current = storage->document((*final_document)->id, DocumentPoint::current());
    REQUIRE(current.has_value());
    REQUIRE(current->has_value());
    CHECK((*current)->value == chord::Json{chord::Json::Object{{"n", chord::Json{3}}}});
    const auto seqs = document_revision_seqs(workspace.path() / "storage.sqlite", (*final_document)->id);
    REQUIRE(seqs.size() >= 3);
    auto first = storage->document((*final_document)->id, DocumentPoint::as_of(seqs[0]));
    auto middle = storage->document((*final_document)->id, DocumentPoint::as_of(seqs[1]));
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    REQUIRE(middle.has_value());
    REQUIRE(middle->has_value());
    CHECK((*first)->value == chord::Json{chord::Json::Object{{"n", chord::Json{1}}}});
    CHECK((*middle)->value == chord::Json{chord::Json::Object{{"n", chord::Json{2}}}});

    auto final_submission = storage->submission(ids.submission);
    REQUIRE(final_submission.has_value());
    REQUIRE(final_submission->has_value());
    CHECK((*final_submission)->status == "unanswered");
    CHECK((*final_submission)->reason == "aborted");
    CHECK((*final_submission)->request_id == kIssue1086RequestId);
    REQUIRE(storage->close().has_value());
}

TEST_CASE("C++ creates a SQLite store, pi advances it, and C++ finishes it",
        "[durable][sqlite][issue1086][continuation]") {
    TempWorkspace workspace;
    const auto path = workspace.path() / "storage.sqlite";
    auto opened = SqliteStorage::open(path);
    if (!opened) FAIL(opened.error().message << ": " << opened.error().detail);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    auto history = continuation_document();
    REQUIRE(history.has_value());

    TaskDefinitionBuilder builder{std::string{kIssue1086TaskKind}, 1};
    builder.initial([](const chord::Json&) { return start_checkpoint(); });
    auto definition = builder.build();
    REQUIRE(definition.has_value());
    std::vector<TaskDefinition> tasks;
    tasks.push_back(std::move(*definition));
    auto harness_opened = consume(Harness::open(*storage, HarnessOptions{.tasks = std::move(tasks)}));
    if (!harness_opened.has_value()) FAIL(harness_opened.error().message << ": " << harness_opened.error().detail);
    REQUIRE(harness_opened.has_value());
    auto harness = std::move(harness_opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());

    TaskId task_id{};
    SubmissionId submission_id{};
    auto committed = consume(harness->session().commit([&](Transaction& tx) -> support::Expected<void> {
        auto created = tx.create_task(*harness->task_definition(kIssue1086TaskKind),
                chord::Json{chord::Json::Object{{"n", chord::Json{7}}}},
                TaskOptions{.conversation_id = root.value()});
        if (!created) return std::unexpected(created.error());
        task_id = *created;
        auto submission = tx.create_submission(SubmissionRecord{.conversation_id = root.value(),
                .request_id = std::string{kIssue1086RequestId},
                .type = "input",
                .status = "queued"});
        if (!submission) return std::unexpected(submission.error());
        submission_id = submission->id;
        auto draft = tx.doc(*history, DocumentAddress{.conversation_id = root.value()});
        if (!draft) return std::unexpected(draft.error());
        if (auto changed = draft->set(member_path("n"), chord::Json{1}); !changed)
            return std::unexpected(changed.error());
        return {};
    }));
    if (!committed.has_value()) FAIL(committed.error().message << ": " << committed.error().detail);
    REQUIRE(committed.has_value());
    auto closed = consume(harness->close());
    REQUIRE(closed.has_value());
    if (const char* capture = std::getenv("CCH_1086_CPP_STORE_OUT"); capture != nullptr) {
        std::error_code error;
        std::filesystem::copy_file(path, capture, std::filesystem::copy_options::overwrite_existing, error);
        REQUIRE_FALSE(error);
    }
    harness.reset();
    storage.reset();

    run_pi_sqlite(workspace.path(), "advance", task_id, submission_id, root.value());

    auto reopened = SqliteStorage::open(path);
    if (!reopened) FAIL(reopened.error().message << ": " << reopened.error().detail);
    REQUIRE(reopened.has_value());
    storage = std::move(*reopened);
    auto advanced_task = storage->task(task_id);
    REQUIRE(advanced_task.has_value());
    REQUIRE(advanced_task->has_value());
    CHECK((*advanced_task)->state.status == "running");
    REQUIRE((*advanced_task)->state.checkpoint.has_value());
    CHECK(*(*advanced_task)->state.checkpoint == finish_checkpoint());
    auto scanned_tasks = storage->scan_tasks(10);
    REQUIRE(scanned_tasks.has_value());
    bool scanned_kind = false;
    for (const TaskRecord& record : *scanned_tasks)
        scanned_kind = scanned_kind || record.kind == kIssue1086TaskKind;
    CHECK(scanned_kind);
    const DocumentAddress address{.conversation_id = root.value()};
    auto advanced_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(advanced_document.has_value());
    REQUIRE(advanced_document->has_value());
    auto advanced_value = storage->document((*advanced_document)->id, DocumentPoint::current());
    REQUIRE(advanced_value.has_value());
    REQUIRE(advanced_value->has_value());
    CHECK((*advanced_value)->value == chord::Json{chord::Json::Object{{"n", chord::Json{2}}}});
    auto advanced_submission = storage->submission_by_request(root.value(), kIssue1086RequestId);
    REQUIRE(advanced_submission.has_value());
    REQUIRE(advanced_submission->has_value());
    CHECK((*advanced_submission)->status == "unanswered");
    CHECK((*advanced_submission)->reason == "aborted");

    auto finished = finish_with_cpp(*storage, *history, task_id);
    if (!finished.has_value()) FAIL(finished.error().message << ": " << finished.error().detail);
    REQUIRE(finished.has_value());
    storage.reset();

    auto final = SqliteStorage::open(path);
    if (!final) FAIL(final.error().message << ": " << final.error().detail);
    REQUIRE(final.has_value());
    storage = std::move(*final);
    auto final_task = storage->task(task_id);
    REQUIRE(final_task.has_value());
    REQUIRE(final_task->has_value());
    CHECK((*final_task)->state.status == "terminal");
    REQUIRE((*final_task)->state.outcome.has_value());
    CHECK((*final_task)->state.outcome->status == "completed");
    CHECK((*final_task)->state.outcome->result == chord::Json{17});
    auto final_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(final_document.has_value());
    REQUIRE(final_document->has_value());
    auto final_value = storage->document((*final_document)->id, DocumentPoint::current());
    REQUIRE(final_value.has_value());
    REQUIRE(final_value->has_value());
    CHECK((*final_value)->value == chord::Json{chord::Json::Object{{"n", chord::Json{3}}}});
    const auto seqs = document_revision_seqs(path, (*final_document)->id);
    REQUIRE(seqs.size() >= 3);
    auto first = storage->document((*final_document)->id, DocumentPoint::as_of(seqs[0]));
    auto middle = storage->document((*final_document)->id, DocumentPoint::as_of(seqs[1]));
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    REQUIRE(middle.has_value());
    REQUIRE(middle->has_value());
    CHECK((*first)->value == chord::Json{chord::Json::Object{{"n", chord::Json{1}}}});
    CHECK((*middle)->value == chord::Json{chord::Json::Object{{"n", chord::Json{2}}}});
    REQUIRE(storage->close().has_value());
}

TEST_CASE("a failed C++ commit on a pi SQLite store rolls back and pi still continues",
        "[durable][sqlite][issue1086][continuation]") {
    TempWorkspace workspace;
    run_pi_sqlite(workspace.path(), "create");
    const PiSqliteIds ids = read_pi_sqlite_ids(workspace.path());
    const auto path = workspace.path() / "storage.sqlite";

    auto opened = SqliteStorage::open(path);
    if (!opened) FAIL(opened.error().message << ": " << opened.error().detail);
    REQUIRE(opened.has_value());
    std::unique_ptr<SqliteStorage> storage = std::move(*opened);
    auto history = continuation_document();
    REQUIRE(history.has_value());
    const DocumentAddress address{.conversation_id = ids.conversation};
    auto task_record = storage->task(ids.task);
    REQUIRE(task_record.has_value());
    REQUIRE(task_record->has_value());
    auto document_record = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(document_record.has_value());
    REQUIRE(document_record->has_value());
    auto submission_record = storage->submission(ids.submission);
    REQUIRE(submission_record.has_value());
    REQUIRE(submission_record->has_value());

    sqlite3* raw = open_database(path);
    const std::string next_seq_before = pragma_text(raw, "SELECT next_seq FROM durable_metadata WHERE singleton=1");
    const std::string next_id_before = pragma_text(raw, "SELECT next_id FROM durable_metadata WHERE singleton=1");
    const std::string ids_before = pragma_text(raw, "SELECT COUNT(*) FROM record_ids");
    sqlite3_close(raw);

    execute_sql(path,
            "CREATE TRIGGER fail_1086_task BEFORE UPDATE ON tasks WHEN NEW.id=" + std::to_string(ids.task.value) +
                    " BEGIN SELECT RAISE(ABORT, 'injected 1086 task failure'); END");
    auto session = Session::create(*storage);
    bool callback_ran = false;
    auto failed = consume(session->commit([&](Transaction& tx) -> support::Expected<void> {
        callback_ran = true;
        auto draft = tx.doc(*history, address);
        if (!draft) return std::unexpected(draft.error());
        if (auto changed = draft->set(member_path("n"), chord::Json{2}); !changed)
            return std::unexpected(changed.error());
        TaskRecord updated = **task_record;
        updated.state = TaskState{.status = "running", .checkpoint = finish_checkpoint()};
        if (auto set = tx.set_task(std::move(updated)); !set) return std::unexpected(set.error());
        if (auto settled = tx.settle_submission(
                    ids.submission, SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                !settled)
            return std::unexpected(settled.error());
        return {};
    }));
    REQUIRE(callback_ran);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().detail.find("injected 1086 task failure") != std::string::npos);
    CHECK(session->failed());
    session.reset();
    storage.reset();

    execute_sql(path, "DROP TRIGGER fail_1086_task");
    auto recovered = SqliteStorage::open(path);
    if (!recovered) FAIL(recovered.error().message << ": " << recovered.error().detail);
    REQUIRE(recovered.has_value());
    storage = std::move(*recovered);
    auto rolled_back_task = storage->task(ids.task);
    REQUIRE(rolled_back_task.has_value());
    REQUIRE(rolled_back_task->has_value());
    CHECK((*rolled_back_task)->state.status == "pending");
    REQUIRE((*rolled_back_task)->state.checkpoint.has_value());
    CHECK(*(*rolled_back_task)->state.checkpoint == start_checkpoint());
    auto rolled_back_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(rolled_back_document.has_value());
    REQUIRE(rolled_back_document->has_value());
    CHECK((*rolled_back_document)->id == (*document_record)->id);
    auto rolled_back_value = storage->document((*rolled_back_document)->id, DocumentPoint::current());
    REQUIRE(rolled_back_value.has_value());
    REQUIRE(rolled_back_value->has_value());
    CHECK((*rolled_back_value)->value == chord::Json{chord::Json::Object{{"n", chord::Json{1}}}});
    auto rolled_back_submission = storage->submission(ids.submission);
    REQUIRE(rolled_back_submission.has_value());
    REQUIRE(rolled_back_submission->has_value());
    CHECK((*rolled_back_submission)->status == "queued");
    raw = open_database(path);
    CHECK(pragma_text(raw, "SELECT next_seq FROM durable_metadata WHERE singleton=1") == next_seq_before);
    CHECK(pragma_text(raw, "SELECT next_id FROM durable_metadata WHERE singleton=1") == next_id_before);
    CHECK(pragma_text(raw, "SELECT COUNT(*) FROM record_ids") == ids_before);
    sqlite3_close(raw);
    storage.reset();

    run_pi_sqlite(workspace.path(), "advance", ids.task, ids.submission, ids.conversation);
    auto advanced = SqliteStorage::open(path);
    if (!advanced) FAIL(advanced.error().message << ": " << advanced.error().detail);
    REQUIRE(advanced.has_value());
    storage = std::move(*advanced);
    auto advanced_task = storage->task(ids.task);
    REQUIRE(advanced_task.has_value());
    REQUIRE(advanced_task->has_value());
    CHECK((*advanced_task)->state.status == "running");
    REQUIRE((*advanced_task)->state.checkpoint.has_value());
    CHECK(*(*advanced_task)->state.checkpoint == finish_checkpoint());
    auto advanced_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(advanced_document.has_value());
    REQUIRE(advanced_document->has_value());
    auto advanced_value = storage->document((*advanced_document)->id, DocumentPoint::current());
    REQUIRE(advanced_value.has_value());
    REQUIRE(advanced_value->has_value());
    CHECK((*advanced_value)->value == chord::Json{chord::Json::Object{{"n", chord::Json{2}}}});
    auto advanced_submission = storage->submission(ids.submission);
    REQUIRE(advanced_submission.has_value());
    REQUIRE(advanced_submission->has_value());
    CHECK((*advanced_submission)->status == "unanswered");
    CHECK((*advanced_submission)->reason == "aborted");

    auto finished = finish_with_cpp(*storage, *history, ids.task);
    if (!finished.has_value()) FAIL(finished.error().message << ": " << finished.error().detail);
    REQUIRE(finished.has_value());
    storage.reset();

    auto final = SqliteStorage::open(path);
    if (!final) FAIL(final.error().message << ": " << final.error().detail);
    REQUIRE(final.has_value());
    storage = std::move(*final);
    auto final_task = storage->task(ids.task);
    REQUIRE(final_task.has_value());
    REQUIRE(final_task->has_value());
    CHECK((*final_task)->state.status == "terminal");
    REQUIRE((*final_task)->state.outcome.has_value());
    CHECK((*final_task)->state.outcome->status == "completed");
    CHECK((*final_task)->state.outcome->result == chord::Json{17});
    auto final_document = storage->find_document(kIssue1086DocumentKind, address, DocumentPoint::current());
    REQUIRE(final_document.has_value());
    REQUIRE(final_document->has_value());
    auto final_value = storage->document((*final_document)->id, DocumentPoint::current());
    REQUIRE(final_value.has_value());
    REQUIRE(final_value->has_value());
    CHECK((*final_value)->value == chord::Json{chord::Json::Object{{"n", chord::Json{3}}}});
    REQUIRE(storage->close().has_value());
}
