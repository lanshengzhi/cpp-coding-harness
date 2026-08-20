#include <cch/agent/harness/session/SessionStore.hpp>

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

harness::session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return {"session-store-test", "2026-07-05T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

ai::MessageVariant user_message(std::string content) {
    return ai::MessageVariant{ai::user_text_message(std::move(content))};
}

std::string user_text(const ai::MessageVariant& message) {
    return ai::text_from_user_message(std::get<ai::UserMessage>(message));
}

} // namespace

TEST_CASE(
    "Session Store facade is a closed concrete move-only value",
    "[harness][session][store][issue464]") {
    using Store = harness::session::SessionStore;
    static_assert(!std::is_abstract_v<Store>);
    static_assert(std::is_final_v<Store>);
    static_assert(std::is_move_constructible_v<Store>);
    static_assert(std::is_move_assignable_v<Store>);
    static_assert(!std::is_copy_constructible_v<Store>);
    static_assert(!std::is_copy_assignable_v<Store>);
    SUCCEED();
}

TEST_CASE(
    "in-memory Session Store defaults the session metadata",
    "[harness][session][store][issue494]") {
    // The zero-argument form mints an empty SessionMetadata header.
    auto store = harness::session::SessionStore::in_memory();
    CHECK(store.metadata().session_id.empty());
    CHECK(store.metadata().created_at.empty());
    CHECK(store.metadata().workspace.empty());
    CHECK(store.metadata().provider.empty());
    CHECK(store.metadata().model.empty());
    CHECK_FALSE(store.metadata().parent_session.has_value());

    // The accessor contract is a borrowed const reference (spec #493).
    static_assert(std::is_same_v<
                  decltype(std::declval<const harness::session::SessionStore&>().metadata()),
                  const harness::session::SessionMetadata&>);
}

TEST_CASE(
    "Session Store metadata returns the construction header for both alternatives",
    "[harness][session][store][issue494]") {
    tests::TempWorkspace workspace;

    auto memory = harness::session::SessionStore::in_memory(metadata_for(workspace));
    CHECK(memory.metadata().session_id == "session-store-test");
    CHECK(memory.metadata().created_at == "2026-07-05T00:00:00Z");
    CHECK(memory.metadata().workspace == workspace.path());
    CHECK(memory.metadata().provider == "fake");
    CHECK(memory.metadata().model == "fake-model");

    const auto path = workspace.path() / "meta.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    CHECK(created->metadata().session_id == "session-store-test");
    CHECK(created->metadata().model == "fake-model");

    // An opened store reports the persisted header.
    auto opened = harness::session::SessionStore::open_existing(path);
    REQUIRE(opened.has_value());
    CHECK(opened->metadata().session_id == "session-store-test");
    CHECK(opened->metadata().workspace == workspace.path());
}

TEST_CASE(
    "in-memory Session Store maintains a live tree without a session file",
    "[harness][session][store][issue464][issue490]") {
    tests::TempWorkspace workspace;
    auto store = harness::session::SessionStore::in_memory(metadata_for(workspace));

    CHECK_FALSE(store.path().has_value());
    CHECK(store.leaf_id().empty());
    CHECK(store.tree().empty());

    REQUIRE(store.append(user_message("hello")).has_value());
    CHECK(store.append_model_change(std::nullopt, "fake", "fake-model").has_value());
    CHECK(store.append_thinking_level_change(std::nullopt, "high").has_value());
    CHECK(store.append_session_info(std::nullopt, "name").has_value());

    // Appends advance the live tree exactly like pi's non-persisting
    // SessionManager: every navigable entry becomes the leaf.
    const auto leaf = store.leaf_id();
    CHECK_FALSE(leaf.empty());
    CHECK(store.get_entry(leaf).has_value());
    CHECK(store.get_session_name() == "name");

    CHECK(store.append_label_change(std::nullopt, leaf, "labeled").has_value());
    CHECK(store.get_label(leaf) == "labeled");

    // Context reconstruction answers from the live tree.
    const auto context = store.build_context();
    REQUIRE(context.messages.size() >= 1);
    CHECK(user_text(context.messages.front()) == "hello");
    CHECK(context.provider == "fake");
    CHECK(context.model == "fake-model");
    CHECK(context.thinking_level == "high");

    CHECK(store
              .append_compaction(
                  std::nullopt,
                  "summary",
                  "first-kept",
                  1000,
                  std::nullopt,
                  std::nullopt)
              .has_value());

    // A root leaf marker moves the live leaf to the root position; the next
    // append starts a new root (pi `resetLeaf`).
    REQUIRE(store.append_leaf(std::nullopt, std::nullopt).has_value());
    CHECK(store.leaf_id().empty());
    REQUIRE(store.append(user_message("after reset")).has_value());
    CHECK(store.tree().size() == 2);
    CHECK_FALSE(store.path().has_value());
}

TEST_CASE(
    "JSONL Session Store writes typed entries and mirrors them into the live tree",
    "[harness][session][store][issue464][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "facade.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.path().has_value());
    CHECK(*store.path() == path);

    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append_model_change(std::nullopt, "fake", "fake-model-2").has_value());
    REQUIRE(store.append_thinking_level_change(std::nullopt, "high").has_value());
    REQUIRE(store.append_session_info(std::nullopt, "facade session").has_value());

    // The live tree mirrors every append: the leaf is the last navigable
    // entry and each entry is retrievable by id.
    const auto leaf = store.leaf_id();
    CHECK_FALSE(leaf.empty());
    const auto leaf_entry = store.get_entry(leaf);
    REQUIRE(leaf_entry.has_value());
    CHECK(leaf_entry->kind == harness::session::SessionEntryKind::SessionInfo);
    CHECK(store.get_session_name() == "facade session");
    CHECK(store.effective_parent_id(leaf).has_value());

    REQUIRE(store.append_label_change(std::nullopt, "missing", "labeled").has_value());
    CHECK(store.get_label(leaf) == std::nullopt);
    REQUIRE(store.append_leaf(std::nullopt, std::nullopt).has_value());
    CHECK(store.leaf_id().empty());

    auto loaded = harness::session::SessionStore::load(path);
    REQUIRE(loaded.has_value());

    std::vector<harness::session::SessionEntryKind> kinds;
    for (const auto& entry : loaded->entries) {
        kinds.push_back(entry.kind);
    }
    CHECK(
        kinds ==
        std::vector<harness::session::SessionEntryKind>{
            harness::session::SessionEntryKind::Header,
            harness::session::SessionEntryKind::Message,
            harness::session::SessionEntryKind::ModelChange,
            harness::session::SessionEntryKind::ThinkingLevelChange,
            harness::session::SessionEntryKind::SessionInfo,
            harness::session::SessionEntryKind::Label,
            harness::session::SessionEntryKind::Leaf,
        });
    REQUIRE(loaded->messages.size() == 1);
    CHECK(user_text(loaded->messages[0]) == "first");
}

TEST_CASE(
    "live tree queries answer without re-reading the session file",
    "[harness][session][store][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "cached.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    REQUIRE(store.append_session_info(std::nullopt, "cached session").has_value());

    const auto leaf = store.leaf_id();
    const auto topology = store.tree();
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);

    // Removing the file proves the queries serve from the cached live tree:
    // nothing re-reads or re-parses the JSONL from disk.
    std::error_code remove_ec;
    std::filesystem::remove(path, remove_ec);
    REQUIRE_FALSE(remove_ec);

    CHECK(store.leaf_id() == leaf);
    CHECK(store.tree().size() == topology.size());
    const auto cached_context = store.build_context();
    REQUIRE(cached_context.messages.size() == 2);
    CHECK(user_text(cached_context.messages[0]) == "first");
    CHECK(user_text(cached_context.messages[1]) == "second");
    CHECK(store.get_session_name() == "cached session");
    const auto branch = store.get_branch();
    REQUIRE(branch.size() == 3);
    CHECK(branch.front().entry_id == leaf);
}

TEST_CASE(
    "tree_snapshot returns a self-consistent roots and leaf pair",
    "[harness][session][store][issue491]") {
    tests::TempWorkspace workspace;
    auto store = harness::session::SessionStore::in_memory(metadata_for(workspace));
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    // The snapshot's leaf is reachable in its own roots (one linear chain).
    auto snapshot = store.tree_snapshot();
    REQUIRE(snapshot.roots.size() == 1);
    CHECK(snapshot.leaf_id == store.leaf_id());
    bool found_leaf = false;
    std::vector<const harness::session::SessionTreeNode*> stack;
    for (const auto& root : snapshot.roots) stack.push_back(&root);
    while (!stack.empty()) {
        const auto* node = stack.back();
        stack.pop_back();
        if (node->entry.entry_id == snapshot.leaf_id) {
            found_leaf = true;
        }
        for (const auto& child : node->children) stack.push_back(&child);
    }
    CHECK(found_leaf);
}

TEST_CASE(
    "Session Store branch summary persists and joins the active-path context",
    "[harness][session][store][issue494]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "branch-summary.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.append(user_message("branch root")).has_value());
    // pi `appendBranchSummary`: the summary of the abandoned branch hangs
    // on the active path and becomes the leaf.
    REQUIRE(store
                .append_branch_summary(
                    std::nullopt,
                    "abandoned-leaf",
                    "summary of abandoned branch",
                    std::nullopt,
                    std::nullopt)
                .has_value());

    // The live tree mirrors the append without re-reading the file.
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);
    CHECK(std::holds_alternative<ai::UserMessage>(context.messages[0]));
    const auto* summary = std::get_if<ai::BranchSummaryMessage>(&context.messages[1]);
    REQUIRE(summary != nullptr);
    CHECK(summary->summary == "summary of abandoned branch");

    // The summary entry is durable: a fresh load parses it back.
    auto loaded = harness::session::SessionStore::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 3);
    const auto& persisted = loaded->entries[2];
    CHECK(persisted.kind == harness::session::SessionEntryKind::BranchSummary);
    const auto* value =
        std::get_if<harness::session::BranchSummaryEntryValue>(&persisted.value);
    REQUIRE(value != nullptr);
    CHECK(value->from_id == "abandoned-leaf");
    CHECK(value->summary == "summary of abandoned branch");
}

TEST_CASE(
    "in-memory Session Store branch summary joins the live tree without disk I/O",
    "[harness][session][store][issue494]") {
    auto store = harness::session::SessionStore::in_memory();
    REQUIRE(store.append(user_message("branch root")).has_value());
    REQUIRE(store
                .append_branch_summary(
                    std::nullopt,
                    "abandoned-leaf",
                    "in-memory branch summary",
                    std::nullopt,
                    std::nullopt)
                .has_value());

    CHECK_FALSE(store.path().has_value());
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);
    const auto* summary = std::get_if<ai::BranchSummaryMessage>(&context.messages[1]);
    REQUIRE(summary != nullptr);
    CHECK(summary->summary == "in-memory branch summary");
}

TEST_CASE(
    "branch and reset_leaf move the live leaf",
    "[harness][session][store][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "branch.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const auto second_leaf = store.leaf_id();

    // Branch back to the first message: the live context shrinks to the
    // first path (pi `branch`).
    const auto entries = store.entries();
    REQUIRE(entries.size() == 2);
    const auto first_id = entries.front().entry_id;
    REQUIRE(store.branch(first_id).has_value());
    CHECK(store.leaf_id() == first_id);
    const auto branched = store.build_context();
    REQUIRE(branched.messages.size() == 1);
    CHECK(user_text(branched.messages[0]) == "first");

    CHECK_FALSE(store.branch("missing-entry").has_value());

    // Root position: the context is empty until the next append (pi
    // `resetLeaf`). In the C++ flat-file shape the chain break is carried
    // by the persisted root leaf marker, exactly like the runtime's
    // navigate-to-root flow.
    store.reset_leaf();
    CHECK(store.leaf_id().empty());
    CHECK(store.build_context().messages.empty());

    REQUIRE(store.append_leaf(std::nullopt, std::nullopt).has_value());
    REQUIRE(store.append(user_message("restarted")).has_value());
    CHECK(store.leaf_id() != second_leaf);
    const auto restarted = store.build_context();
    REQUIRE(restarted.messages.size() == 1);
    CHECK(user_text(restarted.messages[0]) == "restarted");

    // The incrementally maintained tree equals a from-disk rebuild.
    auto reopened = harness::session::SessionStore::open_existing(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->leaf_id() == store.leaf_id());
    CHECK(reopened->build_context().messages.size() == 1);
}

TEST_CASE(
    "a persisted leaf marker moves the live leaf and parents later messages",
    "[harness][session][store][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "marker.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const auto entries = store.entries();
    REQUIRE(entries.size() == 2);
    const auto first_id = entries.front().entry_id;

    REQUIRE(store.append_leaf(std::nullopt, first_id).has_value());
    CHECK(store.leaf_id() == first_id);

    // Marker discipline: the next message hangs under the marker target and
    // becomes the leaf (pi `_appendEntry`).
    REQUIRE(store.append(user_message("third")).has_value());
    const auto third_id = store.leaf_id();
    REQUIRE(third_id != first_id);
    const auto third = store.get_entry(third_id);
    REQUIRE(third.has_value());
    CHECK(third->parent_id == first_id);

    const auto branch = store.get_branch();
    REQUIRE(branch.size() == 2);
    CHECK(branch[0].entry_id == third_id);
    CHECK(branch[1].entry_id == first_id);

    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);
    CHECK(user_text(context.messages[0]) == "first");
    CHECK(user_text(context.messages[1]) == "third");
}

TEST_CASE(
    "a leaf marker targeting a missing entry keeps the current live leaf",
    "[harness][session][store][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "stale-marker.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.append(user_message("first")).has_value());
    const auto leaf = store.leaf_id();

    // The marker persists (the wire shape is legal), but the live tree keeps
    // the last navigable position, exactly like a fresh load's fallback
    // (select_active_leaf_target).
    REQUIRE(store.append_leaf(std::nullopt, "missing-entry").has_value());
    CHECK(store.leaf_id() == leaf);

    auto reopened = harness::session::SessionStore::open_existing(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->leaf_id() == leaf);
}

TEST_CASE(
    "the live tree matches a fresh load after marker, typed, and message appends",
    "[harness][session][store][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "consistency.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const auto first_id = store.entries().front().entry_id;

    // Navigate back, then interleave a typed append and a message append:
    // the incrementally maintained tree must equal a from-disk rebuild.
    REQUIRE(store.append_leaf(std::nullopt, first_id).has_value());
    REQUIRE(store.append_thinking_level_change(std::nullopt, "high").has_value());
    REQUIRE(store.append(user_message("third")).has_value());
    REQUIRE(store.append_model_change(std::nullopt, "fake", "fake-model-2").has_value());

    auto reopened = harness::session::SessionStore::open_existing(path);
    REQUIRE(reopened.has_value());

    CHECK(store.leaf_id() == reopened->leaf_id());
    const auto live_entries = store.entries();
    const auto loaded_entries = reopened->entries();
    REQUIRE(live_entries.size() == loaded_entries.size());
    for (std::size_t index = 0; index < live_entries.size(); ++index) {
        CHECK(live_entries[index].kind == loaded_entries[index].kind);
        CHECK(live_entries[index].entry_id == loaded_entries[index].entry_id);
        CHECK(live_entries[index].parent_id == loaded_entries[index].parent_id);
        CHECK(live_entries[index].timestamp == loaded_entries[index].timestamp);
    }

    const auto live_context = store.build_context();
    const auto loaded_context = reopened->build_context();
    REQUIRE(live_context.messages.size() == loaded_context.messages.size());
    for (std::size_t index = 0; index < live_context.messages.size(); ++index) {
        CHECK(user_text(live_context.messages[index]) ==
              user_text(loaded_context.messages[index]));
    }
    CHECK(live_context.thinking_level == loaded_context.thinking_level);
}

TEST_CASE(
    "open_existing builds the live tree from the persisted entries exactly once",
    "[harness][session][store][issue490]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "reopen.jsonl";
    std::string marked_leaf;
    {
        auto created = harness::session::SessionStore::create_new(
            path, metadata_for(workspace));
        REQUIRE(created.has_value());
        auto store = std::move(*created);
        REQUIRE(store.append(user_message("first")).has_value());
        REQUIRE(store.append(user_message("second")).has_value());
        marked_leaf = store.entries().front().entry_id;
        REQUIRE(store.append_leaf(std::nullopt, marked_leaf).has_value());
    }

    auto opened = harness::session::SessionStore::open_existing(path);
    REQUIRE(opened.has_value());
    auto store = std::move(*opened);
    CHECK(store.leaf_id() == marked_leaf);

    // The tree was built at open: later file removal does not affect it.
    std::error_code remove_ec;
    std::filesystem::remove(path, remove_ec);
    REQUIRE_FALSE(remove_ec);
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 1);
    CHECK(user_text(context.messages[0]) == "first");
}

TEST_CASE(
    "moved Session Store keeps appending to the same session file",
    "[harness][session][store][issue464]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "moved.jsonl";
    auto created = harness::session::SessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);

    auto moved = std::move(store);
    REQUIRE(moved.append(user_message("after move")).has_value());
    REQUIRE(moved.path().has_value());
    CHECK(*moved.path() == path);
    CHECK(moved.build_context().messages.size() == 1);

    auto loaded = harness::session::SessionStore::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 1);
    CHECK(user_text(loaded->messages[0]) == "after move");
}
