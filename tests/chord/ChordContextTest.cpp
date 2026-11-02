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
// The frozen record observes `value` and the abort-signal presence/aborted/reason of the root, both
// children, the inheritor and three ordered observations; every one of those is asserted here.
TEST_CASE("a child cancellation is isolated while parent cancellation reaches an already-created context",
        "[chord][context][cancellation][issue1060][spec]") {
    const auto key = create_context_key<std::string>("audit-1057-context");
    const auto root = with_context_value(key, std::string{"root"}, background_context());

    auto child = with_cancel(root);
    auto sibling = with_cancel(root);

    // Frozen root: value "root", no abort signal, not aborted, no reason.
    REQUIRE(root.value(key).has_value());
    CHECK(*root.value(key) == "root");
    CHECK_FALSE(root.stop_token().stop_possible());
    CHECK_FALSE(root.aborted());
    CHECK_FALSE(root.abort_reason().has_value());

    child.cancel("child-only");
    // Frozen observation "child": aborted with "child-only" while the root stays un-aborted.
    CHECK(child.context().stop_token().stop_possible());
    CHECK(child.context().aborted());
    CHECK(child.context().abort_reason() == std::optional<std::string>{"child-only"});
    CHECK_FALSE(root.aborted());
    // Frozen observation "sibling" before its own cancellation: present value, un-aborted.
    REQUIRE(sibling.context().value(key).has_value());
    CHECK(*sibling.context().value(key) == "root");
    CHECK(sibling.context().stop_token().stop_possible());
    CHECK_FALSE(sibling.context().aborted());
    REQUIRE(child.context().value(key).has_value());
    CHECK(*child.context().value(key) == "root");

    sibling.cancel("sibling-only");
    // Frozen observation "sibling-after-cancel".
    CHECK(sibling.context().aborted());
    CHECK(sibling.context().abort_reason() == std::optional<std::string>{"sibling-only"});
    CHECK_FALSE(root.aborted());

    auto parent = with_cancel(background_context());
    auto inheritor = with_abort_signal(parent.context().stop_token(),
            "parent-reason",
            with_context_value(key, std::string{"child"}, background_context()));
    CHECK(inheritor.stop_token().stop_possible());
    CHECK_FALSE(inheritor.aborted());

    parent.cancel("parent-reason");
    // Frozen inheritor: parent reason reaches it while its own value survives.
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

    // Frozen masked: value "cleanup", no abort signal, not aborted, no reason.
    const auto masked = without_abort_signal(context);
    CHECK_FALSE(masked.aborted());
    CHECK_FALSE(masked.abort_reason().has_value());
    CHECK_FALSE(masked.stop_token().stop_possible());
    REQUIRE(masked.value(key).has_value());
    CHECK(*masked.value(key) == "cleanup");

    // Frozen plainBackground: no abort signal, not aborted, no reason.
    CHECK_FALSE(background_context().stop_token().stop_possible());
    CHECK_FALSE(background_context().aborted());
    CHECK_FALSE(background_context().abort_reason().has_value());

    // Frozen withUndefinedSignal: an un-aborted signal is present.
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

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/context-cancellation.json
// "a context wait rejects with the caller abort reason while the underlying
// work still completes". The frozen `expected` object records the wait error
// (AbortError / "The operation was aborted" / ABORT_ERR 20), the work result
// and that the work resolved after the wait rejected; every one of those is
// asserted here.
TEST_CASE("a context wait rejects with the caller abort reason while the underlying work still completes",
        "[chord][context][cancellation][issue1184][spec]") {
    std::stop_source caller;
    const auto waiting_context = with_abort_signal(caller.get_token(), "stop waiting", background_context());

    // The gated work: settled only after the wait has rejected.
    std::optional<std::string> work_result;
    std::optional<support::AsyncCompletion<std::string, support::Error>> settle_work;
    auto work = support::AsyncResult<std::string>{support::AsyncProducer<std::string, support::Error>{
            [&settle_work, &work_result](support::AsyncCompletion<std::string, support::Error> completion) noexcept {
                settle_work.emplace([&work_result, completion = std::move(completion)](
                                            std::expected<std::string, support::Error> outcome) mutable noexcept {
                    if (outcome) work_result = *outcome;
                    completion(std::move(outcome));
                });
            }}};

    std::optional<support::Expected<std::string>> wait_outcome;
    await_with_context(std::move(work), waiting_context)
            .start([&wait_outcome](support::Expected<std::string> outcome) noexcept {
                wait_outcome.emplace(std::move(outcome));
            });
    REQUIRE(settle_work.has_value());
    CHECK_FALSE(wait_outcome.has_value());

    caller.request_stop();
    REQUIRE(wait_outcome.has_value());
    REQUIRE_FALSE(wait_outcome->has_value());
    // Frozen `waitError`: AbortError with the frozen message and ABORT_ERR.
    CHECK(wait_outcome->error().code == support::ErrorCode::Cancelled);
    CHECK(wait_outcome->error().message == "The operation was aborted");
    CHECK(wait_outcome->error().detail == "stop waiting");

    // Frozen `workResult` and `workResolvedAfterWaitRejected`: the work was
    // still running when the wait rejected and it completes afterwards.
    CHECK_FALSE(work_result.has_value());
    (*settle_work)(std::string{"work finished"});
    CHECK(work_result == std::optional<std::string>{"work finished"});
}

// Discriminating case for the row above: a waiter that cancelled its producer
// would never start or never settle the underlying operation. An already
// cancelled caller must still start the work, and the work must still complete
// after the wait rejected.
TEST_CASE("an already cancelled context wait still starts and completes the underlying work",
        "[chord][context][cancellation][issue1184][discriminating]") {
    auto cancelled = with_cancel(background_context());
    cancelled.cancel("already stopped");

    bool work_started = false;
    std::optional<support::AsyncCompletion<int, support::Error>> settle_work;
    auto work = support::AsyncResult<int>{support::AsyncProducer<int, support::Error>{
            [&work_started, &settle_work](support::AsyncCompletion<int, support::Error> completion) noexcept {
                work_started = true;
                settle_work.emplace(std::move(completion));
            }}};

    std::optional<support::Expected<int>> wait_outcome;
    await_with_context(std::move(work), cancelled.context())
            .start([&wait_outcome](
                           support::Expected<int> outcome) noexcept { wait_outcome.emplace(std::move(outcome)); });

    REQUIRE(wait_outcome.has_value());
    REQUIRE_FALSE(wait_outcome->has_value());
    CHECK(wait_outcome->error().code == support::ErrorCode::Cancelled);
    CHECK(wait_outcome->error().detail == "already stopped");
    CHECK(work_started);
    REQUIRE(settle_work.has_value());
    (*settle_work)(7);
    // The dropped work result never replaces the one terminal wait outcome.
    REQUIRE_FALSE(wait_outcome->has_value());
    CHECK(wait_outcome->error().detail == "already stopped");
}

// The other half of the wait: a context that is not cancelled delivers the
// operation's own result, and a context without a cancellation layer returns
// the operation unchanged (pi `awaitWithContext` without an abort signal).
TEST_CASE("a context wait delivers the work result when the caller is not cancelled",
        "[chord][context][cancellation][issue1184][discriminating]") {
    const auto visible = with_abort_signal(std::stop_token{}, std::string{"unused"}, background_context());
    auto waited = await_with_context(support::AsyncResult<int>{support::Expected<int>{11}}, visible);
    std::optional<support::Expected<int>> wait_outcome;
    std::move(waited).start(
            [&wait_outcome](support::Expected<int> outcome) noexcept { wait_outcome.emplace(std::move(outcome)); });
    REQUIRE(wait_outcome.has_value());
    REQUIRE(wait_outcome->has_value());
    CHECK(**wait_outcome == 11);

    auto plain = await_with_context(support::AsyncResult<int>{support::Expected<int>{12}}, background_context());
    std::optional<support::Expected<int>> plain_outcome;
    std::move(plain).start(
            [&plain_outcome](support::Expected<int> outcome) noexcept { plain_outcome.emplace(std::move(outcome)); });
    REQUIRE(plain_outcome.has_value());
    REQUIRE(plain_outcome->has_value());
    CHECK(**plain_outcome == 12);
}
