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
