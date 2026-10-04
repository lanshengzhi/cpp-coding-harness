// Storage conformance suite for the Session Store (pi
// `durable/src/testing/storage-conformance.ts`).
//
// The suite is runner-independent by construction: every case below receives a
// store built by a registered alternative and asserts only properties that ALL
// alternatives must satisfy. Adding a persistence alternative is therefore a
// registration in `kAlternatives`, not a second copy of these cases — which is
// the property that makes this suite worth having: a new backend inherits the
// judgement instead of being asked to re-earn it.
//
// What belongs here is the *shared* contract. Deliberate divergences stay in
// their own tests next door, because a case that says "unless you are the
// in-memory store" is a conformance case that has stopped being one:
// - redaction and unreadable-record refusal are persistence policy
//   (`SessionStoreTest.cpp`, issue665);
// - file existence and re-open durability are the persisting half
//   (`JsonlSessionStoreTest.cpp`).

#include <cch/agent/harness/session/SessionStore.hpp>

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using harness::session::SessionEntryKind;
using harness::session::SessionMetadata;
using harness::session::SessionStore;

SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return SessionMetadata{
        .session_id = "conformance",
        .created_at = "2026-10-04T00:00:00Z",
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

/// One persistence alternative.
///
/// `build` constructs it; `remove_backing_store` destroys whatever the
/// alternative persists to, without touching the store object itself. It
/// exists so a case can sever an alternative's durable backing and keep
/// asserting — which is how "the live tree answers without re-reading" is
/// stated once for both alternatives instead of only for the persisting one.
///
/// Every alternative MUST supply `remove_backing_store`, including one with no
/// durable backing, which registers a no-op. It is called unconditionally, so
/// a null pointer here would be a call through null; requiring the no-op keeps
/// registration total and leaves the case free of any "does this alternative
/// have backing?" test — the same rule that keeps backend knowledge out of the
/// cases themselves.
struct Alternative {
    std::string_view name;
    support::Expected<SessionStore> (*build)(const tests::TempWorkspace&);
    void (*remove_backing_store)(const harness::session::SessionStore&);
};

void remove_jsonl_backing(const harness::session::SessionStore& store) {
    const auto path = store.path();
    REQUIRE(path.has_value());
    std::error_code ec;
    std::filesystem::remove(*path, ec);
    REQUIRE_FALSE(ec);
}

void remove_no_backing(const harness::session::SessionStore&) {
    // Nothing to remove: an in-memory store has no file. Registered as a no-op
    // rather than nullptr so registration stays total and the case can call
    // unconditionally.
}

support::Expected<SessionStore> build_jsonl(const tests::TempWorkspace& workspace) {
    return SessionStore::create_new(
            workspace.path() / "conformance.jsonl", metadata_for(workspace));
}

support::Expected<SessionStore> build_in_memory(const tests::TempWorkspace& workspace) {
    return SessionStore::in_memory(metadata_for(workspace));
}

/// Ordered so the persisting alternative is exercised first: a case that only
/// the persisting half can reach should fail on the cheaper alternative last,
/// not first.
constexpr std::array<Alternative, 2> kAlternatives{Alternative{"jsonl", &build_jsonl, &remove_jsonl_backing},
        Alternative{"in-memory", &build_in_memory, &remove_no_backing}};

/// One conformance case. It receives a live store, the alternative it came
/// from, and that alternative's `remove_backing_store`. Cases must not name an
/// alternative; a case that needs to reach past the store asks the alternative
/// to sever its own backing rather than testing `path()` for nullopt.
using Case = void (*)(SessionStore& store, const Alternative& alternative);

struct NamedCase {
    std::string_view name;
    Case run;
};

// --- The cases -------------------------------------------------------------
//
// Each names a property, not an implementation. The wording matters: a case
// that can only be satisfied by one alternative has been mis-filed and belongs
// in that alternative's own test.

void metadata_is_the_construction_header(SessionStore& store, const Alternative&) {
    const SessionMetadata& header = store.metadata();
    CHECK(header.session_id == "conformance");
    CHECK(header.created_at == "2026-10-04T00:00:00Z");
    CHECK(header.provider == "fake");
    CHECK(header.model == "fake-model");
    CHECK_FALSE(header.parent_session.has_value());
}

void a_fresh_store_is_empty(SessionStore& store, const Alternative&) {
    CHECK(store.entries().empty());
    CHECK(store.tree().empty());
    CHECK(store.leaf_id().empty());
    CHECK_FALSE(store.get_session_name().has_value());
    CHECK(store.build_context().messages.empty());
}

void appends_become_the_live_leaf(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    const std::string first = store.leaf_id();
    CHECK_FALSE(first.empty());
    CHECK(store.get_entry(first).has_value());

    // Every navigable append advances the leaf — the property a caller relies
    // on to keep talking to the session it just wrote to.
    REQUIRE(store.append(user_message("second")).has_value());
    const std::string second = store.leaf_id();
    CHECK(second != first);
    CHECK(store.get_entry(second).has_value());
}

void typed_appends_reach_the_live_tree(SessionStore& store, const Alternative&) {
    REQUIRE(store.append_model_change(std::nullopt, "fake", "fake-model-2").has_value());
    REQUIRE(store.append_thinking_level_change(std::nullopt, "high").has_value());
    REQUIRE(store.append_session_info(std::nullopt, "conformance session").has_value());

    CHECK(store.get_session_name() == "conformance session");
    const auto context = store.build_context();
    CHECK(context.provider == "fake");
    CHECK(context.model == "fake-model-2");
    CHECK(context.thinking_level == "high");
}

void context_rebuild_walks_the_active_leaf_path(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const auto entries = store.entries();
    REQUIRE(entries.size() == 2);

    // Branching back shrinks the context to the branched path: context rebuild
    // follows the leaf, not the entry log.
    REQUIRE(store.branch(entries.front().entry_id).has_value());
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 1);
    CHECK(user_text(context.messages.front()) == "first");
}

void branching_to_a_missing_entry_fails(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    const std::string leaf = store.leaf_id();

    CHECK_FALSE(store.branch("no-such-entry").has_value());
    // A rejected branch must not move the leaf: the caller keeps the session
    // position it had rather than silently losing it.
    CHECK(store.leaf_id() == leaf);
}

void reset_leaf_returns_to_the_root_position(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    // `reset_leaf` is live-tree state only (the C++ leaf-marker durability
    // discipline): it moves the active leaf to the root position without
    // writing anything.
    store.reset_leaf();
    CHECK(store.leaf_id().empty());
    CHECK(store.build_context().messages.empty());

    // The chain break is persisted separately, by the root leaf marker. Only
    // that marker makes the next append start a new root instead of extending
    // the previous history — so a case that asserted "reset_leaf alone forks
    // the chain" would be asserting a property the facade does not claim.
    REQUIRE(store.append_leaf(std::nullopt, std::nullopt).has_value());
    REQUIRE(store.append(user_message("restarted")).has_value());
    CHECK(store.tree().size() == 2);
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 1);
    CHECK(user_text(context.messages.front()) == "restarted");
}

void a_leaf_marker_reparents_later_messages(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const std::string first = store.entries().front().entry_id;

    REQUIRE(store.append_leaf(std::nullopt, first).has_value());
    CHECK(store.leaf_id() == first);

    // The marker decides where the next append hangs, so the branch a caller
    // navigated to is the branch the transcript continues on.
    REQUIRE(store.append(user_message("third")).has_value());
    const auto third = store.get_entry(store.leaf_id());
    REQUIRE(third.has_value());
    CHECK(third->parent_id == first);

    const auto branch = store.get_branch();
    REQUIRE(branch.size() == 2);
    CHECK(branch.front().entry_id == store.leaf_id());
    CHECK(branch.back().entry_id == first);
}

void labels_attach_only_to_the_entry_they_name(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());
    const std::string first = store.entries().front().entry_id;
    const std::string second = store.leaf_id();

    REQUIRE(store.append_label_change(std::nullopt, first, "labeled").has_value());
    CHECK(store.get_label(first) == "labeled");
    // A label is addressed by target id, so labelling one entry must leave
    // every other entry unlabelled rather than leaking onto the current leaf.
    CHECK_FALSE(store.get_label(second).has_value());
}

void a_label_can_be_cleared(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    const std::string leaf = store.leaf_id();

    REQUIRE(store.append_label_change(std::nullopt, leaf, "labeled").has_value());
    REQUIRE(store.append_label_change(std::nullopt, leaf, std::nullopt).has_value());
    CHECK_FALSE(store.get_label(leaf).has_value());
}

void an_empty_retained_tail_is_not_engaged(SessionStore& store, const Alternative&) {
    const harness::session::CompactionEntryValue value{
        .summary = "summary",
        .first_kept_entry_id = "first-kept",
        .tokens_before = 1000,
        .retained_tail = std::vector<ai::MessageVariant>{},
    };
    REQUIRE(store.append_compaction(std::nullopt, value).has_value());

    // Context rebuild branches on the engaged optional, so an engaged-but-empty
    // tail would take the retained-tail path where a reloaded session takes the
    // first-kept path. Normalize at append time instead.
    const auto entries = store.entries();
    REQUIRE(entries.size() == 1);
    const auto& compaction =
            std::get<harness::session::CompactionEntryValue>(entries.front().value);
    CHECK_FALSE(compaction.retained_tail.has_value());
}

void branch_summaries_join_the_active_path(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("branch root")).has_value());
    REQUIRE(store
                    .append_branch_summary(
                            std::nullopt, "abandoned-leaf", "summary", std::nullopt, std::nullopt)
                    .has_value());

    // The summary of an abandoned branch hangs on the path the session is
    // actually on, so the next request still carries what happened there.
    const auto context = store.build_context();
    REQUIRE(context.messages.size() == 2);
    const auto* summary = std::get_if<ai::BranchSummaryMessage>(&context.messages.back());
    REQUIRE(summary != nullptr);
    CHECK(summary->summary == "summary");
}

void the_tree_snapshot_is_self_consistent(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    // The leaf must be reachable inside the roots the same snapshot returned:
    // a caller reading one consistent snapshot must never see a leaf that its
    // own roots do not contain.
    const auto snapshot = store.tree_snapshot();
    CHECK(snapshot.leaf_id == store.leaf_id());
    bool found = false;
    std::vector<const harness::session::SessionTreeNode*> stack;
    for (const auto& root : snapshot.roots) {
        stack.push_back(&root);
    }
    while (!stack.empty()) {
        const auto* node = stack.back();
        stack.pop_back();
        if (node->entry.entry_id == snapshot.leaf_id) {
            found = true;
        }
        for (const auto& child : node->children) {
            stack.push_back(&child);
        }
    }
    CHECK(found);
}

void queries_answer_without_the_session_file(SessionStore& store, const Alternative& alternative) {
    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append(user_message("second")).has_value());

    const std::string leaf = store.leaf_id();
    const auto topology = store.tree();
    const auto context = store.build_context();

    // Severing the durable backing proves the live-tree queries serve from the
    // cache: a store that silently re-read its persistence would fail here
    // even though every value it returns is correct. An alternative with no
    // durable backing skips the step — there is nothing to remove — and is
    // still held to the same assertions.
    alternative.remove_backing_store(store);

    CHECK(store.leaf_id() == leaf);
    CHECK(store.tree().size() == topology.size());
    const auto cached = store.build_context();
    REQUIRE(cached.messages.size() == context.messages.size());
    CHECK(user_text(cached.messages.back()) == "second");
}

void a_moved_store_keeps_appending(SessionStore& store, const Alternative&) {
    REQUIRE(store.append(user_message("first")).has_value());

    // The facade is move-only (issue464); a moved-to store must remain a fully
    // usable owner rather than a husk.
    SessionStore moved = std::move(store);
    REQUIRE(moved.append(user_message("second")).has_value());
    CHECK(moved.build_context().messages.size() == 2);
}

constexpr std::array<NamedCase, 15> kCases{
    NamedCase{"metadata is the construction header", &metadata_is_the_construction_header},
    NamedCase{"a fresh store is empty", &a_fresh_store_is_empty},
    NamedCase{"appends become the live leaf", &appends_become_the_live_leaf},
    NamedCase{"typed appends reach the live tree", &typed_appends_reach_the_live_tree},
    NamedCase{"context rebuild walks the active leaf path",
            &context_rebuild_walks_the_active_leaf_path},
    NamedCase{"branching to a missing entry fails", &branching_to_a_missing_entry_fails},
    NamedCase{"reset_leaf returns to the root position", &reset_leaf_returns_to_the_root_position},
    NamedCase{"a leaf marker reparents later messages", &a_leaf_marker_reparents_later_messages},
    NamedCase{"labels attach only to the entry they name", &labels_attach_only_to_the_entry_they_name},
    NamedCase{"a label can be cleared", &a_label_can_be_cleared},
    NamedCase{"an empty retained tail is not engaged", &an_empty_retained_tail_is_not_engaged},
    NamedCase{"branch summaries join the active path", &branch_summaries_join_the_active_path},
    NamedCase{"the tree snapshot is self-consistent", &the_tree_snapshot_is_self_consistent},
    NamedCase{"queries answer without the session file", &queries_answer_without_the_session_file},
    NamedCase{"a moved store keeps appending", &a_moved_store_keeps_appending},
};

} // namespace

/// Every registered alternative must satisfy every registered case.
///
/// This is the whole point of the suite: a new persistence alternative is
/// added to `kAlternatives` and immediately inherits the judgement, so it
/// cannot ship with a gap that the two existing alternatives happen to share
/// by accident of who wrote them.
TEST_CASE("every persistence alternative satisfies the storage conformance cases",
        "[harness][session][store][conformance][issue464][spec]") {
    for (const auto& alternative : kAlternatives) {
        for (const auto& conformance_case : kCases) {
            DYNAMIC_SECTION(std::string{alternative.name} + " / "
                            + std::string{conformance_case.name}) {
                tests::TempWorkspace workspace;
                auto built = alternative.build(workspace);
                REQUIRE(built.has_value());
                conformance_case.run(*built, alternative);
            }
        }
    }
}

/// Move semantics are part of the contract every alternative inherits through
/// the facade, and they are cheap to state once for all of them.
TEST_CASE("every persistence alternative yields a move-only store",
        "[harness][session][store][conformance][issue464][spec]") {
    static_assert(!std::is_abstract_v<SessionStore>);
    static_assert(std::is_final_v<SessionStore>);
    static_assert(std::is_move_constructible_v<SessionStore>);
    static_assert(!std::is_copy_constructible_v<SessionStore>);

    for (const auto& alternative : kAlternatives) {
        DYNAMIC_SECTION(std::string{alternative.name}) {
            tests::TempWorkspace workspace;
            auto built = alternative.build(workspace);
            REQUIRE(built.has_value());
            SessionStore moved = std::move(*built);
            REQUIRE(moved.append(user_message("after move")).has_value());
            CHECK(moved.leaf_id().size() > 0);
        }
    }
}
