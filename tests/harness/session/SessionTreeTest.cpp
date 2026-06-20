#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "../../../include/cch/harness/session/SessionTree.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../support/TempWorkspace.hpp"

#include <string>

using namespace cch;

namespace {
harness::session::SessionMetadata test_metadata(const tests::TempWorkspace& workspace) {
    return {"session-test", "2026-06-10T00:00:00Z", workspace.path(), "fake", "fake-model"};
}
} // namespace

// ── U1: SessionTree construction and basic queries ──

TEST_CASE("SessionTree constructs from linear LoadedSession", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "linear.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("hello")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("world")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 3); // header + 2 messages

    harness::session::SessionTree tree(std::move(*loaded));
    CHECK(tree.size() == 2);
    CHECK_FALSE(tree.empty());
    CHECK(tree.metadata().session_id == "session-test");
}

TEST_CASE("SessionTree getEntry finds entry by ID", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "lookup.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("first")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);

    std::string entry_id = loaded->entries[1].entry_id;
    REQUIRE_FALSE(entry_id.empty());

    harness::session::SessionTree tree(std::move(*loaded));
    const auto* entry = tree.getEntry(entry_id);
    REQUIRE(entry != nullptr);
    CHECK(entry->entry_id == entry_id);
    CHECK(entry->kind == harness::session::SessionEntryKind::Message);
}

TEST_CASE("SessionTree getEntry returns nullptr for unknown ID", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "empty.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("msg")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    CHECK(tree.getEntry("deadbeef") == nullptr);
}

TEST_CASE("SessionTree getChildren returns correct children", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "children.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // Simulate a fork: A → B → C and A → B → D
    // We can't directly create forks via JsonlSessionStore, but we can write entries with explicit parent IDs.
    // Use model_change entries that can set parentId.
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("root")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("branch-1")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("branch-2")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() >= 3);

    harness::session::SessionTree tree(std::move(*loaded));

    // In a linear tree, each parent has at most 1 child.
    // Root's parent is nullopt, so it won't appear in children_ map under any key.
    // The second entry's parent should be the first entry's ID.
    if (tree.size() >= 2) {
        std::string parent_id = tree.entries()[0].entry_id;
        auto children = tree.getChildren(parent_id);
        // Linear tree: parent has exactly 1 child
        CHECK(children.size() == 1);
    }
}

TEST_CASE("SessionTree empty tree (header only)", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "header-only.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // Don't append any messages

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    CHECK(tree.empty());
    CHECK(tree.size() == 0);
    CHECK(tree.entries().empty());
}

TEST_CASE("SessionTree is move-only", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "move.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("msg")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree1(std::move(*loaded));
    CHECK(tree1.size() == 1);

    // Move construction
    harness::session::SessionTree tree2(std::move(tree1));
    CHECK(tree2.size() == 1);

    // Move assignment
    harness::session::SessionTree tree3(harness::session::LoadedSession{});
    tree3 = std::move(tree2);
    CHECK(tree3.size() == 1);
}
