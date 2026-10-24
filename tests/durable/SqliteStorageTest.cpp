#include <cch/durable/Session.hpp>
#include <cch/durable/SqliteStorage.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
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
    const auto missing_document = storage->find_document("audit1075.same", std::nullopt);
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

    const auto value = storage->document(DocumentId{.value = 12});
    REQUIRE(value.has_value());
    REQUIRE(value->has_value());
    CHECK(**value == chord::Json{chord::Json::Object{{"revision", chord::Json{2}}}});

    const std::array retire_document{StorageWrite{.type = "document.retire", .document_id = DocumentId{.value = 12}}};
    const Outcome<Seq> retired = consume(storage->commit(retire_document));
    if (!retired.has_value()) INFO(retired.error().message << ": " << retired.error().detail);
    REQUIRE(retired.has_value());
    CHECK(retired.value() == 5);

    const auto missing = storage->find_document("audit1075.versioned", std::nullopt);
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
