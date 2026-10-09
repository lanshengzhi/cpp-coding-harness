#include "agent/harness/session/SqliteDatabase.hpp"
#include "agent/harness/session/SqliteSessionStore.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

TEST_CASE("SqliteDatabase manages transactions and statements cleanly", "[agent][durable][sqlite][spec]") {
    auto db = agent::session::SqliteDatabase::open_memory();
    REQUIRE(db);

    REQUIRE(db->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT);"));

    // Insert within transaction
    {
        agent::session::SqliteTransactionGuard txn(*db);
        REQUIRE(db->begin_transaction());
        auto stmt = db->prepare("INSERT INTO test (val) VALUES (?1);");
        REQUIRE(stmt);
        REQUIRE(stmt->bind_text(1, "hello"));
        auto step = stmt->step();
        REQUIRE(step);
        CHECK_FALSE(*step);
        REQUIRE(txn.commit());
    }

    // Verify insertion
    {
        auto stmt = db->prepare("SELECT val FROM test WHERE id = 1;");
        REQUIRE(stmt);
        auto step = stmt->step();
        REQUIRE(step);
        REQUIRE(*step);
        CHECK(stmt->column_text(0) == "hello");
    }

    // Transaction rollback on failure/destruction
    {
        agent::session::SqliteTransactionGuard txn(*db);
        REQUIRE(db->begin_transaction());
        auto stmt = db->prepare("INSERT INTO test (val) VALUES (?1);");
        REQUIRE(stmt);
        REQUIRE(stmt->bind_text(1, "rolled_back"));
        REQUIRE(stmt->step());
        // No commit -> rollback
    }

    // Verify rolled back
    {
        auto stmt = db->prepare("SELECT COUNT(*) FROM test;");
        REQUIRE(stmt);
        REQUIRE(stmt->step());
        CHECK(stmt->column_int64(0) == 1);
    }
}

TEST_CASE("SqliteSessionStore commits batches atomically", "[agent][durable][sqlite][spec]") {
    auto store = agent::session::SqliteSessionStore::open_memory();
    REQUIRE(store);

    const std::vector<agent::session::SqliteSessionStore::EntryPayload> batch{
            {.entry_id = "batch-root",
                    .parent_id = std::nullopt,
                    .type = "user",
                    .payload_json =
                            R"({"id":"batch-root","parentId":null,"type":"message","message":{"role":"user","content":[{"type":"text","text":"hello"}]}})"},
            {.entry_id = "batch-leaf",
                    .parent_id = "batch-root",
                    .type = "assistant",
                    .payload_json =
                            R"({"id":"batch-leaf","parentId":"batch-root","type":"message","message":{"role":"assistant","content":[{"type":"text","text":"hi"}],"api":"chat","provider":"openai","model":"gpt-4","stopReason":"stop","timestamp":1700000000000,"usage":{"input":10,"output":20,"cacheRead":0,"cacheWrite":0,"totalTokens":30,"cost":{"input":0.0,"output":0.0,"cacheRead":0.0,"cacheWrite":0.0,"total":0.0}}}})"},
    };
    REQUIRE(store->append_batch("batch-session", batch));
    auto tree = store->load_session_tree("batch-session");
    REQUIRE(tree);
    REQUIRE(*tree);
    CHECK((*tree)->entries().size() == 2);

    const std::vector<agent::session::SqliteSessionStore::EntryPayload> failing_batch{
            {.entry_id = "would-rollback",
                    .parent_id = std::nullopt,
                    .type = "user",
                    .payload_json = batch.front().payload_json},
            {.entry_id = "batch-root",
                    .parent_id = std::nullopt,
                    .type = "user",
                    .payload_json = batch.front().payload_json},
    };
    CHECK_FALSE(store->append_batch("rollback-session", failing_batch));
    auto rollback_tree = store->load_session_tree("rollback-session");
    REQUIRE(rollback_tree);
    REQUIRE(*rollback_tree);
    CHECK((*rollback_tree)->entries().empty());
}

TEST_CASE("SqliteSessionStore atomically commits entries and inbox items",
        "[agent][durable][sqlite][session][issue935]") {
    auto store = agent::session::SqliteSessionStore::open_memory();
    REQUIRE(store);

    const agent::session::SqliteSessionStore::EntryPayload entry{.entry_id = "admitted-entry",
            .parent_id = std::nullopt,
            .type = "user",
            .payload_json = R"({"id":"admitted-entry","type":"message"})"};
    const agent::session::SqliteSessionStore::InboxPayload inbox{.id = "inbox-one",
            .kind = agent::session::SqliteSessionStore::InboxKind::Steer,
            .payload = "please steer",
            .boundary_entry_id = "admitted-entry"};

    CHECK_FALSE(store->commit_batch_with_inbox("atomic-session", {entry}, {inbox}, true));
    auto tree_after_failure = store->load_session_tree("atomic-session");
    REQUIRE(tree_after_failure);
    CHECK((*tree_after_failure)->entries().empty());
    auto pending_after_failure = store->claim_pending_inbox("atomic-session");
    REQUIRE(pending_after_failure);
    CHECK(pending_after_failure->empty());

    REQUIRE(store->commit_batch_with_inbox("atomic-session", {entry}, {inbox}));
    auto claimed = store->claim_pending_inbox("atomic-session");
    REQUIRE(claimed);
    REQUIRE(claimed->size() == 1);
    CHECK(claimed->front().id == "inbox-one");
    CHECK(claimed->front().boundary_entry_id == "admitted-entry");
    auto second_claim = store->claim_pending_inbox("atomic-session");
    REQUIRE(second_claim);
    CHECK(second_claim->empty());
}

TEST_CASE("SqliteSessionStore reopens with unclaimed inbox items", "[agent][durable][sqlite][session][issue935]") {
    const auto path = std::filesystem::temp_directory_path() / "cch-inbox-reopen-test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    {
        auto store = agent::session::SqliteSessionStore::open(path);
        REQUIRE(store);
        const agent::session::SqliteSessionStore::InboxPayload inbox{.id = "survives-reopen",
                .kind = agent::session::SqliteSessionStore::InboxKind::Write,
                .payload = "persist this",
                .boundary_entry_id = "entry-boundary"};
        REQUIRE(store->enqueue_inbox("reopen-session", inbox));
    }
    auto reopened = agent::session::SqliteSessionStore::open(path);
    REQUIRE(reopened);
    auto pending = reopened->claim_pending_inbox("reopen-session");
    REQUIRE(pending);
    REQUIRE(pending->size() == 1);
    CHECK(pending->front().id == "survives-reopen");
    std::filesystem::remove(path, ignored);
}

TEST_CASE("SqliteSessionStore appends and replays SessionTree identically", "[agent][durable][sqlite][spec]") {
    auto store = agent::session::SqliteSessionStore::open_memory();
    REQUIRE(store);

    const std::string session_id = "test-session-123";
    const std::string entry1 =
            R"({"id":"root1234","parentId":null,"type":"message","message":{"role":"user","content":[{"type":"text","text":"hello"}]}})";
    const std::string entry2 =
            R"({"id":"leaf1234","parentId":"root1234","type":"message","message":{"role":"assistant","content":[{"type":"text","text":"hi"}],"api":"chat","provider":"openai","model":"gpt-4","stopReason":"stop","timestamp":1700000000000,"usage":{"input":10,"output":20,"cacheRead":0,"cacheWrite":0,"totalTokens":30,"cost":{"input":0.0,"output":0.0,"cacheRead":0.0,"cacheWrite":0.0,"total":0.0}}}})";

    REQUIRE(store->append_entry(session_id, "root1234", std::nullopt, "user", entry1));
    REQUIRE(store->append_entry(session_id, "leaf1234", "root1234", "assistant", entry2));

    auto tree = store->load_session_tree(session_id);
    CHECK(tree.has_value());
    if (!tree) {
        FAIL(tree.error().message + " - " + tree.error().detail);
    }
    REQUIRE(tree);
    REQUIRE(*tree);
    CHECK((*tree)->entries().size() == 2);
    CHECK((*tree)->root() != nullptr);
    CHECK((*tree)->root()->entry_id == "root1234");
}
