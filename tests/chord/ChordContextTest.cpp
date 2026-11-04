#include <cch/chord/ChordContext.hpp>

#include <cch/support/JsonAccess.hpp>

#include "support/ChordEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

using namespace cch;
using namespace cch::chord;

namespace {

/// The frozen #1057 context-cancellation observation the receipts register.
[[nodiscard]] cch::tests::FrozenChordScenario context_cancellation_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1057", "context-cancellation.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// Compare one frozen `{value?, present, aborted, reason}` context state with
/// the live context's own observation.
void check_frozen_state(
        const support::JsonValue::object_t& frozen, const Context& context, const ContextKey<std::string>& key) {
    if (const auto value = support::json_string_member(frozen, "value"); value.has_value()) {
        REQUIRE(context.value(key).has_value());
        CHECK(*context.value(key) == *value);
    }
    const auto present = support::json_bool_member(frozen, "present");
    REQUIRE(present.has_value());
    CHECK(context.stop_token().stop_possible() == *present);
    const auto aborted = support::json_bool_member(frozen, "aborted");
    REQUIRE(aborted.has_value());
    CHECK(context.aborted() == *aborted);
    const auto reason = support::json_string_member(frozen, "reason");
    const std::optional<std::string> frozen_reason =
            reason.has_value() ? std::optional<std::string>{std::string{*reason}} : std::nullopt;
    CHECK(context.abort_reason() == frozen_reason);
}

} // namespace

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
// children, the inheritor and three ordered observations; the inputs (root value, child reason,
// parent reason) and every one of those expected fields are read from the artifact below.
TEST_CASE("a child cancellation is isolated while parent cancellation reaches an already-created context",
        "[chord][context][cancellation][issue1060][spec]") {
    const auto frozen = context_cancellation_scenario(
            "a child cancellation is isolated while parent cancellation reaches an already-created child context");
    const auto root_value = frozen.inputs_string("rootValue");
    const auto child_reason = frozen.inputs_string("childReason");
    const auto parent_reason = frozen.inputs_string("parentReason");
    REQUIRE(root_value.has_value());
    REQUIRE(child_reason.has_value());
    REQUIRE(parent_reason.has_value());
    const auto* frozen_root = frozen.object_member("root");
    const auto* frozen_child = frozen.object_member("child");
    const auto* frozen_sibling = frozen.object_member("sibling");
    const auto* frozen_inheritor = frozen.object_member("inheritor");
    const auto* frozen_observations = frozen.array_member("observations");
    REQUIRE(frozen_root != nullptr);
    REQUIRE(frozen_child != nullptr);
    REQUIRE(frozen_sibling != nullptr);
    REQUIRE(frozen_inheritor != nullptr);
    REQUIRE(frozen_observations != nullptr);
    REQUIRE(frozen_observations->size() == 3);

    const auto key = create_context_key<std::string>("audit-1057-context");
    const auto root = with_context_value(key, *root_value, background_context());

    auto child = with_cancel(root);
    auto sibling = with_cancel(root);

    // Frozen root: the typed value, no abort signal, not aborted, no reason.
    check_frozen_state(*frozen_root, root, key);

    struct Observation {
        std::string observer;
        bool aborted;
        std::optional<std::string> reason;
        bool root_aborted;
    };
    std::vector<Observation> observations;
    const auto record = [&observations, &root](std::string_view observer, const Context& context) {
        observations.push_back(Observation{.observer = std::string{observer},
                .aborted = context.aborted(),
                .reason = context.abort_reason(),
                .root_aborted = root.aborted()});
    };

    child.cancel(*child_reason);
    // Frozen observation "child": aborted with the frozen child reason while the root stays
    // un-aborted; the frozen "child" state holds the root value.
    record("child", child.context());
    check_frozen_state(*frozen_child, child.context(), key);
    check_frozen_state(*frozen_root, root, key);
    // Frozen observation "sibling" before its own cancellation: present value, un-aborted.
    record("sibling", sibling.context());
    check_frozen_state(*frozen_child, child.context(), key);

    const auto sibling_reason = support::json_string_member(*frozen_sibling, "reason");
    REQUIRE(sibling_reason.has_value());
    sibling.cancel(std::string{*sibling_reason});
    // Frozen observation "sibling-after-cancel".
    record("sibling-after-cancel", sibling.context());
    check_frozen_state(*frozen_sibling, sibling.context(), key);
    check_frozen_state(*frozen_root, root, key);

    const auto inheritor_value = support::json_string_member(*frozen_inheritor, "value");
    REQUIRE(inheritor_value.has_value());
    auto parent = with_cancel(background_context());
    auto inheritor = with_abort_signal(parent.context().stop_token(),
            *parent_reason,
            with_context_value(key, std::string{*inheritor_value}, background_context()));
    check_frozen_state(*frozen_root, root, key);
    // The frozen inheritor starts un-aborted with a live parent signal.
    CHECK(inheritor.stop_token().stop_possible());
    CHECK_FALSE(inheritor.aborted());

    parent.cancel(*parent_reason);
    // Frozen inheritor: the frozen parent reason reaches it while its own frozen value survives.
    record("inheritor", inheritor);
    check_frozen_state(*frozen_inheritor, inheritor, key);

    // The frozen ordered observations are compared exactly: observer, aborted, reason, rootAborted.
    for (std::size_t index = 0; index < frozen_observations->size(); ++index) {
        const auto* observation = frozen_observations->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(observation != nullptr);
        REQUIRE(index < observations.size());
        const auto observer = support::json_string_member(*observation, "observer");
        const auto aborted = support::json_bool_member(*observation, "aborted");
        const auto reason = support::json_string_member(*observation, "reason");
        const auto root_aborted = support::json_bool_member(*observation, "rootAborted");
        REQUIRE(observer.has_value());
        REQUIRE(aborted.has_value());
        CHECK(observations[index].observer == *observer);
        CHECK(observations[index].aborted == *aborted);
        const std::optional<std::string> frozen_reason =
                reason.has_value() ? std::optional<std::string>{std::string{*reason}} : std::nullopt;
        CHECK(observations[index].reason == frozen_reason);
        // The third frozen observation records no rootAborted; where the field is present it
        // is compared exactly.
        if (root_aborted.has_value()) CHECK(observations[index].root_aborted == *root_aborted);
    }
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/context-cancellation.json
// "withoutAbortSignal removes cancellation while preserving typed values". The frozen input value
// and the masked/plain-background/with-undefined-signal expected states are the artifact's own
// fields.
TEST_CASE("without abort signal masks cancellation while preserving typed values",
        "[chord][context][cancellation][issue1060][spec]") {
    const auto frozen =
            context_cancellation_scenario("withoutAbortSignal removes cancellation while preserving typed values");
    const auto* frozen_masked = frozen.object_member("masked");
    const auto* frozen_plain_background = frozen.object_member("plainBackground");
    const auto* frozen_with_signal = frozen.object_member("withUndefinedSignal");
    REQUIRE(frozen_masked != nullptr);
    REQUIRE(frozen_plain_background != nullptr);
    REQUIRE(frozen_with_signal != nullptr);
    const auto value = frozen.inputs_string("value");
    REQUIRE(value.has_value());

    const auto key = create_context_key<std::string>("value");
    auto parent = with_cancel(background_context());
    const auto context = with_context_value(key, *value, parent.context());

    parent.cancel("cleanup-cancel");
    CHECK(context.aborted());

    // Frozen masked: the frozen typed value survives with no signal, no abort, no reason.
    const auto masked = without_abort_signal(context);
    check_frozen_state(*frozen_masked, masked, key);

    // Frozen plainBackground: no abort signal, not aborted, no reason.
    check_frozen_state(*frozen_plain_background, background_context(), key);

    // Frozen withUndefinedSignal: an un-aborted signal is present.
    const auto visible = with_abort_signal(std::stop_token{}, std::string{"unused"}, background_context());
    check_frozen_state(*frozen_with_signal, visible, key);
}

// The discriminating case for the frozen child/parent cancellation rows
// (fixtures/pi-v1.3.0/audit-1057/context-cancellation.json): the frozen child keeps its own
// first reason after a competing external stop, and the frozen parent reason reaches an
// already-created child. The reasons under test are the frozen inputs' own values.
TEST_CASE("combined cancellation keeps the first reason and reaches existing children",
        "[chord][context][cancellation][ordering][issue1060][spec]") {
    const auto frozen = context_cancellation_scenario(
            "a child cancellation is isolated while parent cancellation reaches an already-created child context");
    const auto child_reason = frozen.inputs_string("childReason");
    const auto parent_reason = frozen.inputs_string("parentReason");
    REQUIRE(child_reason.has_value());
    REQUIRE(parent_reason.has_value());

    std::stop_source external;
    const auto combined = with_abort_signal(external.get_token(), std::string{"external"}, background_context());
    auto child = with_cancel(combined);

    child.cancel(*child_reason);
    external.request_stop();
    // First reason wins: the frozen child reason, not the later external one.
    CHECK(child.context().abort_reason() == std::optional<std::string>{*child_reason});

    // The frozen parent reason reaches an already-created child context (the frozen inheritor).
    auto cancelled_parent = with_cancel(background_context());
    const auto key = create_context_key<std::string>("audit-1057-context");
    auto inheritor = with_abort_signal(cancelled_parent.context().stop_token(),
            *parent_reason,
            with_context_value(key, std::string{"child"}, background_context()));
    cancelled_parent.cancel(*parent_reason);
    CHECK(inheritor.aborted());
    CHECK(inheritor.abort_reason() == std::optional<std::string>{*parent_reason});

    // A late combination over an already-cancelled parent reports the frozen first reason.
    const auto late_combination =
            with_abort_signal(std::stop_token{}, std::string{"later"}, cancelled_parent.context());
    CHECK(late_combination.aborted());
    CHECK(late_combination.abort_reason() == std::optional<std::string>{*parent_reason});
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/context-cancellation.json
// "a context wait rejects with the caller abort reason while the underlying
// work still completes". The frozen `inputs` (abort reason, work result) and the frozen
// `expected` object (wait error AbortError/"The operation was aborted"/ABORT_ERR 20, the work
// result, and the work resolving after the wait rejected) drive every assertion below.
TEST_CASE("a context wait rejects with the caller abort reason while the underlying work still completes",
        "[chord][context][cancellation][issue1184][spec]") {
    const auto frozen = context_cancellation_scenario(
            "a context wait rejects with the caller abort reason while the underlying work still completes");
    const auto abort_reason = frozen.inputs_string("abortReason");
    const auto work_result = frozen.inputs_string("workResult");
    const auto* frozen_wait_error = frozen.object_member("waitError");
    REQUIRE(abort_reason.has_value());
    REQUIRE(work_result.has_value());
    REQUIRE(frozen_wait_error != nullptr);
    const auto frozen_wait_message = support::json_string_member(*frozen_wait_error, "message");
    const auto frozen_wait_code = support::json_number_member(*frozen_wait_error, "code");
    REQUIRE(frozen_wait_message.has_value());
    REQUIRE(frozen_wait_code.has_value());

    std::stop_source caller;
    const auto waiting_context = with_abort_signal(caller.get_token(), *abort_reason, background_context());

    // The gated work: settled only after the wait has rejected.
    std::optional<std::string> work_result_observed;
    std::optional<support::AsyncCompletion<std::string, support::Error>> settle_work;
    auto work = support::AsyncResult<std::string>{support::AsyncProducer<std::string, support::Error>{
            [&settle_work, &work_result_observed](
                    support::AsyncCompletion<std::string, support::Error> completion) noexcept {
                settle_work.emplace([&work_result_observed, completion = std::move(completion)](
                                            std::expected<std::string, support::Error> outcome) mutable noexcept {
                    if (outcome) work_result_observed = *outcome;
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
    // Frozen `waitError`: the frozen message, the DOM ABORT_ERR 20 mapping to
    // this runtime's Cancelled code, and the frozen caller reason as detail.
    CHECK(wait_outcome->error().message == *frozen_wait_message);
    CHECK(*frozen_wait_code == 20.0);
    CHECK(wait_outcome->error().code == support::ErrorCode::Cancelled);
    CHECK(wait_outcome->error().detail == *abort_reason);

    // Frozen `workResult` and `workResolvedAfterWaitRejected`: the work was
    // still running when the wait rejected and it completes afterwards.
    CHECK_FALSE(work_result_observed.has_value());
    (*settle_work)(std::string{*work_result});
    CHECK(work_result_observed == std::optional<std::string>{*work_result});
}

// Discriminating case for the row above: a waiter that cancelled its producer
// would never start or never settle the underlying operation. An already
// cancelled caller must still start the work, and the work must still complete
// after the wait rejected. The wait error's message and the caller-reason
// detail are the frozen waitError/inputs fields of
// fixtures/pi-v1.3.0/audit-1057/context-cancellation.json.
TEST_CASE("an already cancelled context wait still starts and completes the underlying work",
        "[chord][context][cancellation][issue1184][discriminating]") {
    const auto frozen = context_cancellation_scenario(
            "a context wait rejects with the caller abort reason while the underlying work still completes");
    const auto* frozen_wait_error = frozen.object_member("waitError");
    REQUIRE(frozen_wait_error != nullptr);
    const auto frozen_wait_message = support::json_string_member(*frozen_wait_error, "message");
    REQUIRE(frozen_wait_message.has_value());

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
    // The settled wait carries the frozen AbortError message and the caller reason.
    CHECK(wait_outcome->error().message == *frozen_wait_message);
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
