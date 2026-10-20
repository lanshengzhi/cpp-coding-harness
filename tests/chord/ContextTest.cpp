#include <cch/chord/ChordContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <string>

using namespace cch;
using namespace cch::chord;

TEST_CASE("background and todo contexts are distinct empty roots", "[chord][context][issue1060][spec]") {
    const auto key = create_context_key<std::string>("value");

    CHECK(&background_context() != &todo_context());
    CHECK(background_context().to_string() == "[Context BACKGROUND_CONTEXT]");
    CHECK(todo_context().to_string() == "[Context TODO_CONTEXT]");
    CHECK_FALSE(background_context().aborted());
    CHECK_FALSE(todo_context().aborted());
    CHECK_FALSE(background_context().abort_reason().has_value());
    CHECK_FALSE(background_context().value(key).has_value());
    CHECK_FALSE(background_context().stop_token().stop_possible());
}

TEST_CASE("context values layer without modifying their parents", "[chord][context][issue1060][spec]") {
    const auto first_key = create_context_key<std::string>("first");
    const auto second_key = create_context_key<int>("second");

    const auto first = with_context_value(first_key, std::string{"one"}, background_context());
    const auto second = with_context_value(second_key, 2, first);
    const auto replaced = with_context_value(first_key, std::string{"updated"}, second);

    CHECK_FALSE(background_context().value(first_key).has_value());
    REQUIRE(first.value(first_key).has_value());
    CHECK(*first.value(first_key) == "one");
    CHECK_FALSE(first.value(second_key).has_value());
    REQUIRE(second.value(first_key).has_value());
    CHECK(*second.value(first_key) == "one");
    REQUIRE(second.value(second_key).has_value());
    CHECK(*second.value(second_key) == 2);
    REQUIRE(replaced.value(first_key).has_value());
    CHECK(*replaced.value(first_key) == "updated");
    REQUIRE(second.value(first_key).has_value());
    CHECK(*second.value(first_key) == "one");
    CHECK(replaced.to_string() == "[Context BACKGROUND_CONTEXT].WithValue(first).WithValue(second).WithValue(first)");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/context-cancellation.json
// "a child cancellation is isolated while parent cancellation reaches an already-created context".
TEST_CASE("a child cancellation is isolated while parent cancellation reaches an already-created context",
        "[chord][context][cancellation][issue1060][spec]") {
    const auto key = create_context_key<std::string>("audit-1057-context");
    const auto root = with_context_value(key, std::string{"root"}, background_context());

    auto child = with_cancel(root);
    auto sibling = with_cancel(root);

    child.cancel("child-only");
    CHECK(child.context().aborted());
    CHECK(child.context().abort_reason() == std::optional<std::string>{"child-only"});
    CHECK_FALSE(root.aborted());
    CHECK_FALSE(sibling.context().aborted());
    REQUIRE(child.context().value(key).has_value());
    CHECK(*child.context().value(key) == "root");

    sibling.cancel("sibling-only");
    CHECK(sibling.context().abort_reason() == std::optional<std::string>{"sibling-only"});
    CHECK_FALSE(root.aborted());

    auto parent = with_cancel(background_context());
    auto inheritor = with_abort_signal(parent.context().stop_token(),
            "parent-reason",
            with_context_value(key, std::string{"child"}, background_context()));
    CHECK_FALSE(inheritor.aborted());

    parent.cancel("parent-reason");
    CHECK(inheritor.aborted());
    CHECK(inheritor.abort_reason() == std::optional<std::string>{"parent-reason"});
    REQUIRE(inheritor.value(key).has_value());
    CHECK(*inheritor.value(key) == "child");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/context-cancellation.json
// "withoutAbortSignal removes cancellation while preserving typed values".
TEST_CASE("without abort signal masks cancellation while preserving typed values",
        "[chord][context][cancellation][issue1060][spec]") {
    const auto key = create_context_key<std::string>("value");
    auto parent = with_cancel(background_context());
    const auto context = with_context_value(key, std::string{"cleanup"}, parent.context());

    parent.cancel("cleanup-cancel");
    CHECK(context.aborted());

    const auto masked = without_abort_signal(context);
    CHECK_FALSE(masked.aborted());
    CHECK_FALSE(masked.abort_reason().has_value());
    CHECK_FALSE(masked.stop_token().stop_possible());
    REQUIRE(masked.value(key).has_value());
    CHECK(*masked.value(key) == "cleanup");

    const auto visible = with_abort_signal(std::stop_token{}, std::string{"unused"}, background_context());
    CHECK_FALSE(visible.aborted());
    CHECK_FALSE(visible.abort_reason().has_value());
    CHECK(visible.stop_token().stop_possible());
}

TEST_CASE("combined cancellation keeps the first reason and reaches existing children",
        "[chord][context][cancellation][ordering][issue1060][spec]") {
    std::stop_source external;
    const auto combined = with_abort_signal(external.get_token(), std::string{"external"}, background_context());
    auto child = with_cancel(combined);

    child.cancel("child");
    external.request_stop();
    CHECK(child.context().abort_reason() == std::optional<std::string>{"child"});

    auto cancelled_parent = with_cancel(background_context());
    cancelled_parent.cancel("first");
    const auto late_combination =
            with_abort_signal(std::stop_token{}, std::string{"later"}, cancelled_parent.context());
    CHECK(late_combination.aborted());
    CHECK(late_combination.abort_reason() == std::optional<std::string>{"first"});
}
