#include <cch/agent/harness/session/SessionStore.hpp>

#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
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
    "in-memory Session Store no-ops every typed append and has no path",
    "[harness][session][store][issue464]") {
    auto store = harness::session::SessionStore::in_memory();

    CHECK_FALSE(store.path().has_value());
    CHECK(store.append(user_message("hello")).has_value());
    CHECK(store.append_model_change(std::nullopt, "fake", "fake-model").has_value());
    CHECK(store.append_thinking_level_change(std::nullopt, "high").has_value());
    CHECK(store.append_label_change(std::nullopt, "target", std::nullopt).has_value());
    CHECK(store
              .append_compaction(
                  std::nullopt,
                  "summary",
                  "first-kept",
                  1000,
                  std::nullopt,
                  std::nullopt)
              .has_value());
    CHECK(store.append_session_info(std::nullopt, "name").has_value());
    CHECK(store.append_leaf(std::nullopt, std::nullopt).has_value());
    CHECK_FALSE(store.path().has_value());
}

TEST_CASE(
    "JSONL Session Store writes typed entries through the closed facade",
    "[harness][session][store][issue464]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "facade.jsonl";
    auto jsonl = harness::session::JsonlSessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(jsonl.has_value());
    harness::session::SessionStore store{std::move(*jsonl)};

    REQUIRE(store.path().has_value());
    CHECK(*store.path() == path);

    REQUIRE(store.append(user_message("first")).has_value());
    REQUIRE(store.append_model_change(std::nullopt, "fake", "fake-model-2").has_value());
    REQUIRE(store.append_thinking_level_change(std::nullopt, "high").has_value());
    REQUIRE(store.append_session_info(std::nullopt, "facade session").has_value());
    REQUIRE(store.append_label_change(std::nullopt, "missing", "labeled").has_value());
    REQUIRE(store.append_leaf(std::nullopt, std::nullopt).has_value());

    auto loaded = harness::session::JsonlSessionStore::load(path);
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
    CHECK(ai::text_from_user_message(std::get<ai::UserMessage>(loaded->messages[0])) ==
          "first");
}

TEST_CASE(
    "moved Session Store keeps appending to the same session file",
    "[harness][session][store][issue464]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "moved.jsonl";
    auto jsonl = harness::session::JsonlSessionStore::create_new(
        path, metadata_for(workspace));
    REQUIRE(jsonl.has_value());
    harness::session::SessionStore store{std::move(*jsonl)};

    auto moved = std::move(store);
    REQUIRE(moved.append(user_message("after move")).has_value());
    REQUIRE(moved.path().has_value());
    CHECK(*moved.path() == path);

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 1);
    CHECK(
        ai::text_from_user_message(std::get<ai::UserMessage>(loaded->messages[0])) ==
        "after move");
}
