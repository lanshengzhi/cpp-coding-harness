#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "../../../src/harness/session/JsonlSessionStore.hpp"
#include "../../../src/session/JsonlSessionStore.hpp"
#include "../../support/TempWorkspace.hpp"

#include <fstream>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace cch;

namespace {
harness::session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return {"session-test", "2026-06-10T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

agent::Message user_message(std::string content) {
    agent::Message message;
    message.role = agent::Role::User;
    message.content = std::move(content);
    return message;
}
}

TEST_CASE("harness session store loads legacy header and message entries as typed entries", "[harness][session][u6][ae3]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "legacy.jsonl";
    {
        std::ofstream output(path);
        output << R"({"type":"header","version":1,"session_id":"legacy","created_at":"now","workspace":")"
               << workspace.path().string()
               << R"(","provider":"fake","model":"fake-model"})" << '\n';
        output << R"({"type":"message","entry_id":"m1","message":{"role":"user","content":"first","is_error":false,"tool_calls":[]}})" << '\n';
        output << R"({"type":"message","entry_id":"m2","message":{"role":"assistant","content":"second","is_error":false,"tool_calls":[]}})" << '\n';
    }
#if defined(__unix__) || defined(__APPLE__)
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE(loaded.ok());
    CHECK(loaded.value().metadata.session_id == "legacy");
    REQUIRE(loaded.value().messages.size() == 2);
    CHECK(loaded.value().messages[0].content == "first");
    CHECK(loaded.value().messages[1].content == "second");
    REQUIRE(loaded.value().entries.size() == 3);
    CHECK(loaded.value().entries[0].kind == harness::session::SessionEntryKind::Header);
    CHECK(loaded.value().entries[1].kind == harness::session::SessionEntryKind::Message);
    CHECK(loaded.value().entries[1].entry_id == "m1");
}

TEST_CASE("harness session writes remain readable through legacy session facade", "[harness][session][u6]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "new.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store.ok());
    REQUIRE(store.value().append(user_message("hello")) .ok());

    auto loaded = session::JsonlSessionStore::load(path);

    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().messages.size() == 1);
    CHECK(loaded.value().messages[0].content == "hello");
    REQUIRE(loaded.value().entries.size() == 2);
    CHECK(loaded.value().entries[1].kind == session::SessionEntryKind::Message);
}

TEST_CASE("harness session keeps unknown future entries in typed load result", "[harness][session][u6]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "future.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store.ok());
    {
        std::ofstream output(path, std::ios::app);
        output << "{\"type\":\"future\",\"payload\":42}\n";
    }
    REQUIRE(store.value().append(user_message("known")).ok());

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().unknown_lines.size() == 1);
    REQUIRE(loaded.value().entries.size() == 3);
    CHECK(loaded.value().entries[1].kind == harness::session::SessionEntryKind::Unknown);
    CHECK(loaded.value().entries[2].kind == harness::session::SessionEntryKind::Message);
    CHECK(loaded.value().messages[0].content == "known");
}
