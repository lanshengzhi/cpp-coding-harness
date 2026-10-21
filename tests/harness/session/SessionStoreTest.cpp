#include <cch/agent/harness/session/SessionStore.hpp>

#include "support/Json.hpp"
#include "support/SessionSeeding.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    return harness::session::SessionMetadata{
        .session_id = "session-store-test",
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
}

ai::MessageVariant user_message(std::string content) {
    return ai::MessageVariant{ai::user_text_message(std::move(content))};
}

std::string user_text(const ai::MessageVariant& message) {
    return ai::text_from_user_message(std::get<ai::UserMessage>(message));
}

} // namespace

TEST_CASE("Session Store facade is a closed concrete move-only value", "[harness][session][store][issue464][spec]") {
    using Store = harness::session::SessionStore;
    static_assert(!std::is_abstract_v<Store>);
    static_assert(std::is_final_v<Store>);
    static_assert(std::is_move_constructible_v<Store>);
    static_assert(std::is_move_assignable_v<Store>);
    static_assert(!std::is_copy_constructible_v<Store>);
    static_assert(!std::is_copy_assignable_v<Store>);
    SUCCEED();
}

TEST_CASE("in-memory Session Store defaults the session metadata", "[harness][session][store][issue494][spec]") {
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

TEST_CASE("Session Store metadata returns the construction header for both alternatives",
        "[harness][session][store][issue494][spec]") {
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

    // The deferred first flush (ADR 0064) lands the header with the first
    // user message; only then can an opened store report the persisted
    // header.
    REQUIRE(created->append(user_message("seed")).has_value());

    // An opened store reports the persisted header.
    auto opened = harness::session::SessionStore::open_existing(path);
    REQUIRE(opened.has_value());
    CHECK(opened->metadata().session_id == "session-store-test");
    CHECK(opened->metadata().workspace == workspace.path());
}

TEST_CASE("in-memory Session Store maintains a live tree without a session file",
        "[harness][session][store][issue464][issue490][spec]") {
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
                  harness::session::CompactionEntryValue{
                      .summary = "summary",
                      .first_kept_entry_id = "first-kept",
                      .tokens_before = 1000,
                  })
              .has_value());

    // `reset_leaf` moves the live leaf to the root position; the next
    // append starts a new root (pi `resetLeaf`).
    store.reset_leaf();
    CHECK(store.leaf_id().empty());
    REQUIRE(store.append(user_message("after reset")).has_value());
    CHECK(store.tree().size() == 2);
    CHECK_FALSE(store.path().has_value());
}

TEST_CASE("the in-memory alternative keeps entries the JSONL alternative refuses or redacts",
        "[harness][session][store][issue464][issue665][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "alternative-divergence.jsonl";
    auto persisted = harness::session::SessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(persisted);
    auto memory = harness::session::SessionStore::in_memory(metadata_for(workspace));

    // An assistant record without the identity and real timestamp a provider
    // supplies is one the JSONL writer refuses because the parser could not
    // read it back (issue665); the in-memory alternative never touches a wire
    // line, so it keeps the record. Both halves are asserted because either
    // policy changing on its own breaks the property being named.
    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{
            .text = "in-memory keeps sk-live-value",
            .text_signature = std::nullopt,
    });
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    CHECK_FALSE(persisted->append(ai::MessageVariant{assistant}).has_value());
    REQUIRE(memory.append(ai::MessageVariant{assistant}).has_value());

    // Redaction is persistence policy: a session that never writes keeps the
    // caller's summary verbatim, so context rebuild and inspection answer the
    // values the session actually holds.
    const harness::session::CompactionEntryValue compaction{
            .summary = "summary with sk-live-value",
            .first_kept_entry_id = "first-kept",
            .tokens_before = 1000,
    };
    REQUIRE(persisted->append_compaction(std::nullopt, compaction).has_value());
    REQUIRE(memory.append_compaction(std::nullopt, compaction).has_value());

    const auto memory_entries = memory.entries();
    REQUIRE(memory_entries.size() == 2);
    REQUIRE(memory_entries[0].kind == harness::session::SessionEntryKind::Message);
    REQUIRE(memory_entries[0].message.has_value());
    const auto& kept = std::get<ai::AssistantMessage>(*memory_entries[0].message);
    REQUIRE(kept.content.size() == 1);
    CHECK(std::get<ai::TextContent>(kept.content.front()).text == "in-memory keeps sk-live-value");
    const auto& memory_compaction = std::get<harness::session::CompactionEntryValue>(memory_entries[1].value);
    CHECK(memory_compaction.summary == "summary with sk-live-value");

    // The persisted side wrote neither refused record unredacted: the
    // unreadable assistant is absent and the summary reached the file
    // redacted. A regression that stopped redacting at the persistence
    // boundary fails here even though every in-memory assertion above holds.
    // The deferred first flush (ADR 0064) lands the buffered compaction with
    // this user message.
    REQUIRE(persisted->append(user_message("flush")).has_value());
    const auto reloaded = harness::session::SessionStore::load(path);
    REQUIRE(reloaded);
    REQUIRE(reloaded->entries.size() == 3);
    REQUIRE(reloaded->entries[1].kind == harness::session::SessionEntryKind::Compaction);
    const auto& stored = std::get<harness::session::CompactionEntryValue>(reloaded->entries[1].value);
    CHECK(stored.summary.find("sk-live-value") == std::string::npos);
    CHECK(stored.summary != memory_compaction.summary);
}

TEST_CASE("an engaged but empty compaction tail is dropped by both persistence alternatives",
        "[harness][session][store][issue464][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "empty-retained-tail.jsonl";
    auto persisted = harness::session::SessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(persisted);
    auto memory = harness::session::SessionStore::in_memory(metadata_for(workspace));

    const harness::session::CompactionEntryValue value{
            .summary = "summary",
            .first_kept_entry_id = "first-kept",
            .tokens_before = 1000,
            .retained_tail = std::vector<ai::MessageVariant>{},
    };
    REQUIRE(persisted->append_compaction(std::nullopt, value).has_value());
    REQUIRE(memory.append_compaction(std::nullopt, value).has_value());

    // An empty tail must not survive as an engaged optional: context rebuild
    // branches on it, so an engaged-but-empty tail would take the
    // retained-tail path where a reloaded session takes the first-kept path.
    for (const auto* store : {&*persisted, &memory}) {
        const auto entries = store->entries();
        REQUIRE(entries.size() == 1);
        const auto& compaction = std::get<harness::session::CompactionEntryValue>(entries.front().value);
        CHECK_FALSE(compaction.retained_tail.has_value());
    }

    // The live tree above is only a mirror. Re-read the file, because a
    // regression that dropped the tail from the mirror while the wire line
    // still carried an engaged empty tail would satisfy every check above and
    // still diverge from a reloaded session. The deferred first flush (ADR
    // 0064) lands the buffered compaction with this user message.
    REQUIRE(persisted->append(user_message("flush")).has_value());
    const auto reloaded = harness::session::SessionStore::load(path);
    REQUIRE(reloaded);
    REQUIRE(reloaded->entries.size() == 3);
    REQUIRE(reloaded->entries[1].kind == harness::session::SessionEntryKind::Compaction);
    const auto& reloaded_compaction = std::get<harness::session::CompactionEntryValue>(reloaded->entries[1].value);
    CHECK_FALSE(reloaded_compaction.retained_tail.has_value());
}

TEST_CASE("JSONL Session Store writes typed entries and mirrors them into the live tree",
        "[harness][session][store][issue464][issue490][spec]") {
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
    store.reset_leaf();
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
        });
    REQUIRE(loaded->messages.size() == 1);
    CHECK(user_text(loaded->messages[0]) == "first");
}

TEST_CASE("live tree queries answer without re-reading the session file", "[harness][session][store][issue490][spec]") {
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

TEST_CASE("tree_snapshot returns a self-consistent roots and leaf pair", "[harness][session][store][issue491][spec]") {
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

TEST_CASE("Session Store branch summary persists and joins the active-path context",
        "[harness][session][store][issue494][spec]") {
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

TEST_CASE("in-memory Session Store branch summary joins the live tree without disk I/O",
        "[harness][session][store][issue494][spec]") {
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

TEST_CASE("branch and reset_leaf move the live leaf", "[harness][session][store][issue490][spec]") {
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
    // `resetLeaf`). The chain break is runtime-only state, exactly like
    // pi's leaf pointer.
    store.reset_leaf();
    CHECK(store.leaf_id().empty());
    CHECK(store.build_context().messages.empty());

    REQUIRE(store.append(user_message("restarted")).has_value());
    CHECK(store.leaf_id() != second_leaf);
    const auto restarted = store.build_context();
    REQUIRE(restarted.messages.size() == 1);
    CHECK(user_text(restarted.messages[0]) == "restarted");

    // The incrementally maintained tree and a from-disk rebuild agree on
    // which entry is the active leaf: the append after `reset_leaf` is the
    // last file entry, so pi's `_buildIndex` reconstructs the same pointer.
    auto reopened = harness::session::SessionStore::open_existing(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->leaf_id() == store.leaf_id());
    // STEP 1 scope: the reset-to-root POSITION is runtime-only, exactly like
    // pi's in-memory `leafId` — nothing on disk records that the chain
    // restarted. Restoring that position after a restart is the separate,
    // later step (a pi-compatible mechanism, not a private marker), so this
    // test deliberately asserts no context size for the reopened store.
}

TEST_CASE("a branch moves the live leaf and parents later messages", "[harness][session][store][issue490][spec]") {
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

    // pi `branch`: the active leaf moves to an existing entry. The move is
    // runtime-only state — the session file keeps only entries.
    REQUIRE(store.branch(first_id).has_value());
    CHECK(store.leaf_id() == first_id);

    // The branch decides where the next append hangs (pi `appendMessage`
    // hangs the message under the current leaf).
    REQUIRE(store.append(user_message("third")).has_value());
    const auto third_id = store.leaf_id();
    REQUIRE(third_id != first_id);
    const auto third = store.get_entry(third_id);
    REQUIRE(third.has_value());
    CHECK(cch::harness::session::parent_entry_id(third->parent_id) == std::optional<std::string>{first_id});

    const auto branch = store.get_branch();
    REQUIRE(branch.size() == 2);
    CHECK(branch[0].entry_id == third_id);
    CHECK(branch[1].entry_id == first_id);

    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);
    CHECK(user_text(context.messages[0]) == "first");
    CHECK(user_text(context.messages[1]) == "third");
}

TEST_CASE("the live tree matches a fresh load after a branch, typed, and message appends",
        "[harness][session][store][issue490][spec]") {
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
    REQUIRE(store.branch(first_id).has_value());
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

TEST_CASE("open_existing builds the live tree from the persisted entries exactly once",
        "[harness][session][store][issue490][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "reopen.jsonl";
    std::string last_entry;
    {
        auto created = harness::session::SessionStore::create_new(
            path, metadata_for(workspace));
        REQUIRE(created.has_value());
        auto store = std::move(*created);
        REQUIRE(store.append(user_message("first")).has_value());
        REQUIRE(store.append(user_message("second")).has_value());
        last_entry = store.leaf_id();
    }

    auto opened = harness::session::SessionStore::open_existing(path);
    REQUIRE(opened.has_value());
    auto store = std::move(*opened);
    // pi `_buildIndex`: reopening restores the active leaf from the file's
    // entry order (the last entry), because the leaf is runtime-only state
    // and is never written to the session file.
    CHECK(store.leaf_id() == last_entry);

    // The tree was built at open: later file removal does not affect it.
    std::error_code remove_ec;
    std::filesystem::remove(path, remove_ec);
    REQUIRE_FALSE(remove_ec);
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);
    CHECK(user_text(context.messages[0]) == "first");
    CHECK(user_text(context.messages[1]) == "second");
}

TEST_CASE("moved Session Store keeps appending to the same session file", "[harness][session][store][issue464][spec]") {
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

TEST_CASE("create_from_entries writes a re-chained branch file the loader reads back",
        "[harness][session][store][issue540][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "branched.jsonl";

    // The fork flow hands over a re-chained entry list, never wire lines.
    std::vector<harness::session::SessionEntry> entries;
    harness::session::SessionEntry first;
    first.kind = harness::session::SessionEntryKind::Message;
    first.entry_id = "entry-first";
    first.parent_id = cch::harness::session::parent_id_from_optional(std::nullopt);
    first.timestamp = 1;
    first.message = user_message("branched");
    entries.push_back(std::move(first));
    harness::session::SessionEntry second;
    second.kind = harness::session::SessionEntryKind::Message;
    second.entry_id = "entry-second";
    second.parent_id = cch::harness::session::parent_entry("entry-first");
    second.timestamp = 2;
    second.message = user_message("chained");
    entries.push_back(std::move(second));

    auto header = metadata_for(workspace);
    header.session_id = "branched-session";
    header.parent_session = workspace.path() / "source.jsonl";
    REQUIRE(harness::session::SessionStore::create_from_entries(path, std::move(header), entries).has_value());

    auto loaded = harness::session::SessionStore::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->metadata.session_id == "branched-session");
    CHECK(loaded->metadata.parent_session == workspace.path() / "source.jsonl");
    REQUIRE(loaded->messages.size() == 2);
    CHECK(user_text(loaded->messages[0]) == "branched");
    CHECK(user_text(loaded->messages[1]) == "chained");

    // One wire line per entry: the facade terminates each line exactly once.
    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    CHECK(contents.find("\n\n") == std::string::npos);
    CHECK(std::count(contents.begin(), contents.end(), '\n') == 3);
}

TEST_CASE("create_from_entries preserves a foreign entry line and refuses a nested header",
        "[harness][session][store][issue540][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "foreign.jsonl";

    const std::string foreign_line = R"({"type":"retired_entry","id":"entry-foreign"})";
    std::vector<harness::session::SessionEntry> entries;
    harness::session::SessionEntry before;
    before.kind = harness::session::SessionEntryKind::Message;
    before.entry_id = "entry-before";
    before.timestamp = 1;
    before.message = user_message("before");
    entries.push_back(std::move(before));
    harness::session::SessionEntry foreign;
    foreign.kind = harness::session::SessionEntryKind::Unknown;
    foreign.entry_id = "entry-foreign";
    foreign.raw_line = foreign_line;
    entries.push_back(std::move(foreign));
    // A foreign entry that is not last is the terminator boundary: the
    // reader's `raw_line` carries no newline, so the facade must add it or
    // the next entry concatenates onto one malformed JSON line.
    harness::session::SessionEntry after;
    after.kind = harness::session::SessionEntryKind::Message;
    after.entry_id = "entry-after";
    after.parent_id = cch::harness::session::parent_entry("entry-before");
    after.timestamp = 2;
    after.message = user_message("after");
    entries.push_back(std::move(after));

    REQUIRE(harness::session::SessionStore::create_from_entries(path, metadata_for(workspace), entries).has_value());
    auto loaded = harness::session::SessionStore::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 4);
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::Unknown);
    REQUIRE(loaded->unknown_lines.size() == 1);
    CHECK(loaded->unknown_lines.front() == foreign_line);
    REQUIRE(loaded->messages.size() == 2);
    CHECK(user_text(loaded->messages[0]) == "before");
    CHECK(user_text(loaded->messages[1]) == "after");

    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    CHECK(contents.find("\n\n") == std::string::npos);
    CHECK(std::count(contents.begin(), contents.end(), '\n') == 4);

    // The header comes from metadata alone, so a nested one is rejected
    // rather than written as a second session header.
    std::vector<harness::session::SessionEntry> with_header;
    with_header.push_back(loaded->entries.front());
    auto rejected = harness::session::SessionStore::create_from_entries(
            workspace.path() / "nested-header.jsonl", metadata_for(workspace), with_header);
    REQUIRE_FALSE(rejected.has_value());
    // The rejection precedes the journal, so no half-written session file is
    // left at the target path.
    std::error_code missing_ec;
    CHECK_FALSE(std::filesystem::exists(workspace.path() / "nested-header.jsonl", missing_ec));
}

// ── pi wire shape: one root, a real parent chain, explicit null only at a
// root (pi `appendMessage` writes `parentId: this.leafId` and `_appendEntry`
// then sets `leafId = entry.id`; pi never infers a parent on read) ──

namespace {

/// Every non-header line's `parentId` field, in file order, as the wire
/// spells it: `std::nullopt` for an absent field, otherwise the JSON text.
[[nodiscard]] std::vector<std::optional<std::string>> wire_parent_ids(const std::string& contents) {
    std::vector<std::optional<std::string>> parents;
    std::size_t start = 0;
    while (start < contents.size()) {
        const auto end = contents.find('\n', start);
        const auto line = contents.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? contents.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        auto parsed = support::read_json(line);
        REQUIRE(parsed.has_value());
        const auto* object = parsed->get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        if (auto type = object->find("type"); type != object->end() && type->second.get_if<std::string>() != nullptr &&
                                              *type->second.get_if<std::string>() == "session") {
            continue;
        }
        const auto found = object->find("parentId");
        if (found == object->end()) {
            parents.push_back(std::nullopt);
        } else if (found->second.get_if<support::JsonValue::null_t>() != nullptr) {
            parents.push_back(std::string{"null"});
        } else {
            const auto* text = found->second.get_if<std::string>();
            REQUIRE(text != nullptr);
            parents.push_back(*text);
        }
    }
    return parents;
}

} // namespace

TEST_CASE("a linear session writes one root and a real parent chain",
        "[harness][session][store][issue1044][compat-pi][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "pi-chain.jsonl";
    auto created = harness::session::SessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    REQUIRE(store.append(user_message("third")).has_value());

    // The wire: exactly one explicit null (the root), then a real chain. pi
    // writes this shape and reads it back as one branch; a null on every
    // entry is the un-migrated v1 shape pi would rewrite.
    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto parents = wire_parent_ids(contents);
    REQUIRE(parents.size() == 3);
    CHECK(parents[0] == std::optional<std::string>{"null"});
    CHECK(parents[1] == std::optional<std::string>{store.entries()[0].entry_id});
    CHECK(parents[2] == std::optional<std::string>{store.entries()[1].entry_id});

    // The reloaded tree agrees: one root, a 3-entry branch, no inference.
    auto reopened = harness::session::SessionStore::open_existing(path);
    REQUIRE(reopened.has_value());
    const auto topology = reopened->tree_snapshot();
    REQUIRE(topology.roots.size() == 1);
    CHECK(topology.roots.front().children.size() == 1);
    CHECK(reopened->get_branch().size() == 3);
    CHECK_FALSE(reopened->effective_parent_id(reopened->entries().front().entry_id).has_value());
    CHECK(reopened->effective_parent_id(reopened->entries()[1].entry_id) ==
            std::optional<std::string>{reopened->entries()[0].entry_id});
}

TEST_CASE("a second root after reset_leaf is a root on the wire and on reload",
        "[harness][session][store][issue1044][compat-pi][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "pi-second-root.jsonl";
    auto created = harness::session::SessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    store.reset_leaf();
    REQUIRE(store.append(user_message("second root")).has_value());

    // Two roots on the wire: both entries carry the explicit null pi writes
    // for `resetLeaf`'s new root.
    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto parents = wire_parent_ids(contents);
    REQUIRE(parents.size() == 2);
    CHECK(parents[0] == std::optional<std::string>{"null"});
    CHECK(parents[1] == std::optional<std::string>{"null"});

    // And the reload reads them as two independent roots, not a chain: the
    // reported defect. An explicit null is a root, so it never infers the
    // preceding entry as its parent.
    auto reopened = harness::session::SessionStore::open_existing(path);
    REQUIRE(reopened.has_value());
    const auto entries = reopened->entries();
    REQUIRE(entries.size() == 2);
    const auto second_root_id = entries.back().entry_id;
    CHECK_FALSE(reopened->effective_parent_id(second_root_id).has_value());
    CHECK(reopened->get_branch(second_root_id).size() == 1);
    CHECK(reopened->tree_snapshot().roots.size() == 2);
}

TEST_CASE("a branch target is the next append's wire parent", "[harness][session][store][issue1044][compat-pi][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "pi-branch-parent.jsonl";
    auto created = harness::session::SessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(created.has_value());
    auto store = std::move(*created);
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const auto first_id = store.entries().front().entry_id;
    REQUIRE(store.branch(first_id).has_value());
    REQUIRE(store.append(user_message("branched")).has_value());

    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto parents = wire_parent_ids(contents);
    REQUIRE(parents.size() == 3);
    CHECK(parents[0] == std::optional<std::string>{"null"});
    CHECK(parents[1] == std::optional<std::string>{first_id});
    // The branch target, not the file's previous entry (pi `appendMessage`).
    CHECK(parents[2] == std::optional<std::string>{first_id});
}

TEST_CASE("an explicit parentId null is a root and an absent field still infers the chain",
        "[harness][session][tree][issue1044][compat-pi][spec]") {
    // The reader's two shapes, hand-written so the writer cannot decide the
    // answer: a missing `parentId` is the legacy flat file whose chain comes
    // from the entry order; an explicit null is pi's root.
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "reader-shapes.jsonl";
    const auto header = tests::session_header_line(metadata_for(workspace));
    workspace.write("reader-shapes.jsonl",
            header + "\n" +
                    R"({"type":"message","id":"legacy01","timestamp":"2026-06-10T00:00:01.000Z","message":{"role":"user","content":[{"type":"text","text":"legacy"}],"timestamp":1}})" +
                    "\n" +
                    R"({"type":"message","id":"legacy02","timestamp":"2026-06-10T00:00:02.000Z","message":{"role":"user","content":[{"type":"text","text":"legacy child"}],"timestamp":2}})" +
                    "\n");
    tests::make_session_file_private(path);

    auto loaded = harness::session::SessionStore::open_existing(path);
    REQUIRE(loaded.has_value());
    // Missing parentId keeps the legacy linear-chain inference.
    CHECK(loaded->effective_parent_id("legacy02") == std::optional<std::string>{"legacy01"});
    CHECK(loaded->get_branch("legacy02").size() == 2);
    CHECK(loaded->tree_snapshot().roots.size() == 1);

    // An explicit null on a non-first entry is a root: no inference.
    const auto null_path = workspace.path() / "reader-null.jsonl";
    workspace.write("reader-null.jsonl",
            header + "\n" +
                    R"({"type":"message","id":"null0001","parentId":"missing1","timestamp":"2026-06-10T00:00:01.000Z","message":{"role":"user","content":[{"type":"text","text":"orphan"}],"timestamp":1}})" +
                    "\n" +
                    R"({"type":"message","id":"null0002","parentId":null,"timestamp":"2026-06-10T00:00:02.000Z","message":{"role":"user","content":[{"type":"text","text":"root"}],"timestamp":2}})" +
                    "\n");
    tests::make_session_file_private(null_path);

    auto null_loaded = harness::session::SessionStore::open_existing(null_path);
    REQUIRE(null_loaded.has_value());
    // pi's orphan rule lives in the tree build, not in the parent lookup: an
    // explicit id that names no entry is still the entry's recorded parent
    // (pi's `entry.parentId`), and `getTree` makes the unresolvable node a
    // root. Both entries here are roots for different reasons.
    const auto orphan_topology = null_loaded->tree_snapshot();
    REQUIRE(orphan_topology.roots.size() == 2);
    // An explicit null is a root even though it follows another entry: no
    // inference, and its branch is just itself.
    CHECK_FALSE(null_loaded->effective_parent_id("null0002").has_value());
    CHECK(null_loaded->get_branch("null0002").size() == 1);
    // The orphan keeps pi's wire parent and its own single-entry branch.
    CHECK(null_loaded->effective_parent_id("null0001") == std::optional<std::string>{"missing1"});
    CHECK(null_loaded->get_branch("null0001").size() == 1);
}
