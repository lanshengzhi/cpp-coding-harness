#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
#include "durable/GatedStorage.hpp"
#include "durable/Outcome.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using namespace cch::durable;
using namespace cch::tests::durable;

namespace {

constexpr std::string_view kN6Replay =
        "a watch acquired at an older revision replays every op-bearing committed frame to the current revision";
constexpr std::string_view kN7Buffered =
        "a document watch acquired before start buffers committed frames and delivers them in commit order";

/// The frozen document kind of the captured watch scenarios.
constexpr std::string_view kFramesDocumentKind = "audit1063.frames";

/// One frozen expectation, loaded once per case.
[[nodiscard]] FrozenScenario frozen(std::string_view name) {
    auto scenario = FrozenScenario::load(name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen document definition of the captured watch scenarios: the frozen
/// artifact's initial value is `{value, items, nested}` and the frozen
/// definition is Session-scoped.
[[nodiscard]] DocumentDefinition frames_document() {
    auto definition = define_document(
            DocumentDefinitionBuilder{std::string{kFramesDocumentKind}}, std::move_only_function<chord::Json()>{[] {
                return chord::Json{chord::Json::Object{{"value", chord::Json{0.0}},
                        {"items", chord::Json{chord::Json::Array{chord::Json{std::string{"a"}}}}},
                        {"nested", chord::Json{chord::Json::Object{{"count", chord::Json{0.0}}}}}}};
            }});
    REQUIRE(definition.has_value());
    return std::move(*definition);
}

/// The path of one member of a document value.
[[nodiscard]] chord::Path member_path(std::string key) { return chord::Path{chord::path_key(std::move(key))}; }

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a watch acquired at an older revision replays every op-bearing committed
// frame to the current revision".
//
// Discriminating case: the watch acquired at the older revision must receive
// every later committed frame with its exact operation batch, and replaying
// those batches onto the acquisition value must reproduce the current
// committed snapshot exactly. An implementation that delivers only the newest
// value, or that delivers frames without their operations, fails this case.
TEST_CASE("a watch acquired at an older revision replays every op-bearing committed frame to the current revision",
        "[durable][session][watch][replay][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN6Replay);

    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = frames_document();

    // The first commit creates the document.
    REQUIRE(consume(session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("value"), chord::Json{1.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());

    // The watch is acquired at the older revision.
    Outcome<std::optional<DocumentWatch>> watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(watch.has_value());
    REQUIRE(watch.value().has_value());

    std::vector<DocumentFrame> frames;
    REQUIRE(watch.value().value().start(
            [&frames](DocumentFrame frame, chord::Context) { frames.push_back(std::move(frame)); }));

    // The frozen artifact's acquisition value is the older revision: the watch
    // holds the revision it was acquired at until a frame is delivered.
    const auto* frozen_acquisition = scenario.object_member("acquisitionValue");
    REQUIRE(frozen_acquisition != nullptr);
    const chord::Json acquisition_value = watch.value().value().value();
    CHECK(json_equals(acquisition_value, *frozen_acquisition));

    // Two later commits carry operations.
    REQUIRE(consume(session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("value"), chord::Json{2.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());
    REQUIRE(consume(session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto spliced = draft->splice(member_path("items"), 1, 0, {chord::Json{std::string{"c"}}}); !spliced) {
            return std::unexpected(std::move(spliced.error()));
        }
        if (auto set = draft->set(chord::Path{chord::path_key("nested"), chord::path_key("count")}, chord::Json{1.0});
                !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());

    // The frozen artifact records the delivered frames and their operations.
    const auto* frozen_frames = scenario.array_member("frames");
    REQUIRE(frozen_frames != nullptr);
    REQUIRE(frames.size() == frozen_frames->size());
    CHECK(frames.size() == 2);

    // Every delivered frame carries operations.
    const auto frozen_non_empty = scenario.member("nonEmptyFrameOps");
    REQUIRE(frozen_non_empty != nullptr);
    REQUIRE(frozen_non_empty->holds<bool>());
    CHECK(frozen_non_empty->get<bool>());
    for (const DocumentFrame& frame : frames) {
        CHECK_FALSE(frame.ops.empty());
    }

    // Replaying the delivered operation batches onto the acquisition value
    // reproduces the current committed snapshot exactly: the frozen artifact
    // records `replayMatchesFinal: true`.
    const auto* frozen_replay = scenario.member("replayMatchesFinal");
    REQUIRE(frozen_replay != nullptr);
    REQUIRE(frozen_replay->holds<bool>());
    CHECK(frozen_replay->get<bool>());

    auto replayed = acquisition_value;
    for (const DocumentFrame& frame : frames) {
        auto applied = chord::apply_immutable(replayed, frame.ops);
        REQUIRE(applied.has_value());
        replayed = std::move(*applied);
    }

    auto snapshot = consume(session->snapshot(definition, DocumentAddress{}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    const auto* frozen_final = scenario.object_member("finalSnapshot");
    REQUIRE(frozen_final != nullptr);
    CHECK(json_equals(replayed, *frozen_final));
    CHECK(json_equals(*snapshot.value(), *frozen_final));

    // The watch's value is the latest delivered revision once the frames were
    // delivered (frozen `CommittedWatch.value`).
    CHECK(json_equals(watch.value().value().value(), *frozen_final));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1063/session-transactions.json
// "a document watch acquired before start buffers committed frames and
// delivers them in commit order".
//
// Discriminating case: the frames committed before the watch is started must
// be buffered and delivered in commit order, none lost and none duplicated.
// An implementation that drops the buffered frames, or that delivers them in
// reverse order, fails this case.
TEST_CASE("a document watch acquired before start buffers committed frames and delivers them in commit order",
        "[durable][session][watch][buffering][issue1063][spec]") {
    const FrozenScenario scenario = frozen(kN7Buffered);

    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = frames_document();

    REQUIRE(consume(session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("value"), chord::Json{1.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());

    Outcome<std::optional<DocumentWatch>> watch = consume(session->watch_doc(definition, DocumentAddress{}));
    REQUIRE(watch.has_value());
    REQUIRE(watch.value().has_value());

    // Two commits land while the watch is not started.
    REQUIRE(consume(session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto set = draft->set(member_path("value"), chord::Json{2.0}); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());
    REQUIRE(consume(session->commit([&](Transaction& transaction) -> support::Expected<void> {
        auto draft = transaction.doc(definition, DocumentAddress{});
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        if (auto spliced = draft->splice(member_path("items"), 1, 0, {chord::Json{std::string{"c"}}}); !spliced) {
            return std::unexpected(std::move(spliced.error()));
        }
        if (auto set = draft->set(chord::Path{chord::path_key("nested"), chord::path_key("count")}, chord::Json{1.0});
                !set) {
            return std::unexpected(std::move(set.error()));
        }
        return {};
    })).has_value());

    // The watch still holds its acquisition revision before it is started.
    const auto* frozen_acquisition = scenario.object_member("acquisitionValue");
    REQUIRE(frozen_acquisition != nullptr);
    CHECK(json_equals(watch.value().value().value(), *frozen_acquisition));

    // Starting the watch delivers the buffered frames in commit order.
    std::vector<DocumentFrame> deliveries;
    REQUIRE(watch.value().value().start(
            [&deliveries](DocumentFrame frame, chord::Context) { deliveries.push_back(std::move(frame)); }));

    const auto* frozen_deliveries = scenario.array_member("deliveries");
    REQUIRE(frozen_deliveries != nullptr);
    const auto frozen_delivery_count = scenario.number_member("deliveryCount");
    REQUIRE(frozen_delivery_count.has_value());
    CHECK(static_cast<double>(deliveries.size()) == *frozen_delivery_count);
    REQUIRE(deliveries.size() == frozen_deliveries->size());

    // The first delivered frame carries the first committed value; the second
    // carries the second. The frozen artifact records the delivered values.
    const auto* frozen_delivered_values = scenario.array_member("deliveredValues");
    REQUIRE(frozen_delivered_values != nullptr);
    REQUIRE(frozen_delivered_values->size() == deliveries.size());
    for (std::size_t index = 0; index < deliveries.size(); ++index) {
        const auto* expected = frozen_deliveries->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(expected != nullptr);
        const auto* expected_value = support::json_object_member(*expected, "value");
        REQUIRE(expected_value != nullptr);
        CHECK(json_equals(deliveries[index].value, *expected_value));
    }
}

// ── Context-scoped watch, acquisition and close (issue #1184) ───────────────
//
// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/context-cancellation.json.
// Every expectation below is read from that artifact's own `expected` object,
// so a changed frozen observation changes what the C++ seam must meet.

namespace {

constexpr std::string_view kContextCancellationArtifact = "fixtures/pi-v1.3.0/audit-1057/context-cancellation.json";

/// The document definition of the captured context-cancellation scenarios: a
/// Session-scoped version-1 document with initial `{value: 0}`.
[[nodiscard]] DocumentDefinition context_document(std::string kind) {
    auto definition = define_document(DocumentDefinitionBuilder{std::move(kind)},
            std::move_only_function<chord::Json()>{
                    [] { return chord::Json{chord::Json::Object{{"value", chord::Json{0.0}}}}; }});
    REQUIRE(definition.has_value());
    return std::move(*definition);
}

/// Commit one `value` member under the supplied commit context.
[[nodiscard]] support::ExpectedVoid set_document_value(
        Session& session, const DocumentDefinition& definition, double value, const chord::Context& context) {
    return consume(session.commit(
                           [&definition, value](Transaction& transaction) -> support::Expected<void> {
                               auto draft = transaction.doc(definition, DocumentAddress{});
                               if (!draft) {
                                   return std::unexpected(std::move(draft.error()));
                               }
                               if (auto set = draft->set(member_path("value"), chord::Json{value}); !set) {
                                   return std::unexpected(std::move(set.error()));
                               }
                               return {};
                           },
                           context))
            .result;
}

/// One delivered frame as the frozen `audit-1057` watch scenarios observe it:
/// its value, whether the commit context carried an abort signal, and the
/// commit context's typed value under the scenario's key.
struct ObservedFrame {
    chord::Json value;
    bool abort_signal_present{false};
    std::optional<std::string> context_value;
};

/// The frozen `{name, message}` fields of one captured abort error.
struct FrozenAbortError {
    std::string name;
    std::string message;
};

[[nodiscard]] std::optional<FrozenAbortError> frozen_abort_error(
        const FrozenScenario& scenario, std::string_view member_name) {
    const auto* error = scenario.object_member(member_name);
    if (error == nullptr) return std::nullopt;
    const auto name = support::json_string_member(*error, "name");
    const auto message = support::json_string_member(*error, "message");
    if (!name || !message) return std::nullopt;
    return FrozenAbortError{std::string{*name}, std::string{*message}};
}

} // namespace

// Frozen evidence: "cancelling a watch ends delivery without aborting a
// completed callback or later commits".
//
// The case also separates the frame context: the commit's typed values reach
// the observer without the committer's abort signal. An implementation that
// hands the raw commit context to the listener reports a present abort signal,
// and one that hands the acquisition context reports a missing context value.
TEST_CASE("cancelling a watch ends delivery without aborting a completed callback or later commits",
        "[durable][session][watch][cancellation][issue1184][spec]") {
    const auto scenario = FrozenScenario::load(kContextCancellationArtifact,
            "cancelling a watch ends delivery without aborting a completed callback or later commits");
    REQUIRE(scenario.has_value());

    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = context_document("1057.watch-cancel");
    REQUIRE(set_document_value(*session, definition, 0.0, chord::background_context()).has_value());

    const auto commit_key = chord::create_context_key<std::string>("audit-1057-watch-commit");
    auto scope = chord::with_cancel(chord::background_context());
    auto acquired = consume(session->watch_doc(definition, DocumentAddress{}, scope.context()));
    REQUIRE(acquired.has_value());
    REQUIRE(acquired.value().has_value());
    auto& watch = *acquired.value();

    std::vector<ObservedFrame> frames;
    REQUIRE(watch.start([&frames, &commit_key](DocumentFrame frame, chord::Context context) {
        frames.push_back(ObservedFrame{.value = std::move(frame.value),
                .abort_signal_present = context.stop_token().stop_possible(),
                .context_value = context.value(commit_key)});
    }));

    // The commit carries the frozen commit context value.
    REQUIRE(set_document_value(*session,
            definition,
            2.0,
            chord::with_context_value(commit_key, std::string{"commit-context"}, chord::background_context()))
                    .has_value());

    // Cancelling the watch's context ends delivery; later commits still land.
    scope.cancel("stop watching");
    REQUIRE(set_document_value(*session, definition, 3.0, chord::background_context()).has_value());

    // Frozen `frames`: one frame, its value, a null abort signal and the
    // commit context value.
    const auto* frozen_frames = scenario->array_member("frames");
    REQUIRE(frozen_frames != nullptr);
    REQUIRE(frames.size() == frozen_frames->size());
    REQUIRE(frames.size() == 1);
    const auto* frozen_frame = frozen_frames->at(0).get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_frame != nullptr);
    const auto* frozen_frame_value = support::json_object_member(*frozen_frame, "value");
    REQUIRE(frozen_frame_value != nullptr);
    CHECK(json_equals(frames[0].value, *frozen_frame_value));
    const auto* frozen_signal = support::json_member(*frozen_frame, "abortSignal");
    REQUIRE(frozen_signal != nullptr);
    REQUIRE(frozen_signal->holds<std::nullptr_t>());
    CHECK_FALSE(frames[0].abort_signal_present);
    const auto frozen_context_value = support::json_string_member(*frozen_frame, "contextValue");
    REQUIRE(frozen_context_value.has_value());
    REQUIRE(frames[0].context_value.has_value());
    CHECK(*frames[0].context_value == *frozen_context_value);

    // Frozen `closed: {reason: "cancelled"}`.
    const auto* frozen_closed = scenario->object_member("closed");
    REQUIRE(frozen_closed != nullptr);
    const auto frozen_reason = support::json_string_member(*frozen_closed, "reason");
    REQUIRE(frozen_reason.has_value());
    CHECK(*frozen_reason == "cancelled");
    CHECK(watch.stop() == WatchEndReason::Cancelled);

    // Frozen `valueAfterCancel`: the watch keeps the last delivered revision.
    const auto* frozen_value_after = scenario->member("valueAfterCancel");
    REQUIRE(frozen_value_after != nullptr);
    CHECK(json_equals(watch.value(), *frozen_value_after));

    // Frozen `snapshotAfterCancel`: the later commit reached committed state.
    const auto* frozen_snapshot = scenario->member("snapshotAfterCancel");
    REQUIRE(frozen_snapshot != nullptr);
    auto snapshot = consume(session->snapshot(definition, DocumentAddress{}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(json_equals(*snapshot.value(), *frozen_snapshot));

    // A cancelled watch never delivers its buffered frames either, and its
    // terminal reason is observable from `stop()` alone.
    auto cancelled_scope = chord::with_cancel(chord::background_context());
    auto buffered = consume(session->watch_doc(definition, DocumentAddress{}, cancelled_scope.context()));
    REQUIRE(buffered.has_value());
    REQUIRE(buffered.value().has_value());
    REQUIRE(set_document_value(*session, definition, 4.0, chord::background_context()).has_value());
    cancelled_scope.cancel("stop watching");
    CHECK(buffered.value().value().stop() == WatchEndReason::Cancelled);
    std::size_t cancelled_deliveries = 0;
    CHECK_FALSE(buffered.value().value().start(
            [&cancelled_deliveries](DocumentFrame, chord::Context) { cancelled_deliveries += 1; }));
    CHECK(cancelled_deliveries == 0);
}

// Frozen evidence: "cancelling a watch acquisition rejects the wait and leaves
// no write or partial observer".
//
// Discriminating case (the same case's second half): the acquisition itself
// rejects while its committed read is in flight, so no observer is registered
// at all. An implementation that registers the observer before its
// cancellation check returns a watch here and fails.
TEST_CASE("cancelling a watch acquisition rejects the wait and leaves no write or partial observer",
        "[durable][session][watch][cancellation][issue1184][spec]") {
    const auto scenario = FrozenScenario::load(kContextCancellationArtifact,
            "cancelling a watch acquisition rejects the wait and leaves no write or partial observer");
    REQUIRE(scenario.has_value());

    tests::GatedStorage storage;
    auto writer = Session::create(storage);
    const auto definition = context_document("1057.acquire-cancel");
    REQUIRE(set_document_value(*writer, definition, 1.0, chord::background_context()).has_value());
    const std::size_t writes_before = storage.commit_calls();

    // The frozen caller: the wait under the acquisition context rejects with
    // the frozen `acquisitionError`.
    auto acquire_scope = chord::with_cancel(chord::background_context());
    auto session = Session::create(storage);
    storage.on_find_document([&acquire_scope] { acquire_scope.cancel("cancel acquisition"); });
    const auto acquisition = consume(chord::await_with_context(
            session->watch_doc(definition, DocumentAddress{}, acquire_scope.context()), acquire_scope.context()));
    storage.on_find_document({});
    const auto frozen_error = frozen_abort_error(*scenario, "acquisitionError");
    REQUIRE(frozen_error.has_value());
    REQUIRE_FALSE(acquisition.has_value());
    // Frozen `acquisitionError`: AbortError / "The operation was aborted".
    CHECK(frozen_error->name == "AbortError");
    CHECK(acquisition.error().code == support::ErrorCode::Cancelled);
    CHECK(acquisition.error().message == frozen_error->message);
    CHECK(acquisition.error().detail == "cancel acquisition");

    // Frozen `snapshotAfterCancellation`: no write and no observer left.
    const auto* frozen_snapshot = scenario->member("snapshotAfterCancellation");
    REQUIRE(frozen_snapshot != nullptr);
    auto snapshot = consume(session->snapshot(definition, DocumentAddress{}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    CHECK(json_equals(*snapshot.value(), *frozen_snapshot));
    CHECK(storage.commit_calls() == writes_before);
}

// Discriminating case for the acquisition row: the acquisition itself rejects
// while its committed read is in flight, so it registers no observer at all.
// An implementation that registers the observer before its cancellation check
// returns a watch here and fails; an implementation that writes during the
// acquisition changes the committed snapshot.
TEST_CASE("a cancelled watch acquisition registers no observer",
        "[durable][session][watch][cancellation][issue1184][discriminating]") {
    tests::GatedStorage storage;
    auto writer = Session::create(storage);
    const auto definition = context_document("1057.acquire-cancel");
    REQUIRE(set_document_value(*writer, definition, 1.0, chord::background_context()).has_value());
    const std::size_t writes_before = storage.commit_calls();

    auto scope = chord::with_cancel(chord::background_context());
    auto session = Session::create(storage);
    // The storage cancels the acquiring context while the committed read is in
    // flight, so the cancellation lands after the acquisition started.
    storage.on_find_document([&scope] { scope.cancel("cancel acquisition"); });
    const auto direct = consume(session->watch_doc(definition, DocumentAddress{}, scope.context()));
    storage.on_find_document({});

    REQUIRE_FALSE(direct.has_value());
    CHECK(direct.error().code == support::ErrorCode::Cancelled);
    CHECK(direct.error().detail == "cancel acquisition");
    CHECK(storage.commit_calls() == writes_before);
    auto snapshot = consume(session->snapshot(definition, DocumentAddress{}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    const auto snapshot_json = to_json(*snapshot.value());
    const auto* snapshot_object = support::json_object(snapshot_json);
    REQUIRE(snapshot_object != nullptr);
    const auto snapshot_value = support::json_number_member(*snapshot_object, "value");
    REQUIRE(snapshot_value.has_value());
    CHECK(*snapshot_value == 1.0);
}

// Frozen evidence: "cancelling the close caller rejects close while storage
// cleanup still completes".
//
// Discriminating case (the same case's second half): an already-cancelled
// caller still closes Storage. An implementation that checks the caller's
// context before starting mandatory cleanup skips the storage close and fails.
TEST_CASE("cancelling the close caller rejects close while storage cleanup still completes",
        "[durable][session][close][cancellation][issue1184][spec]") {
    const auto scenario = FrozenScenario::load(kContextCancellationArtifact,
            "cancelling the close caller rejects close while storage cleanup still completes");
    REQUIRE(scenario.has_value());

    tests::GatedStorage storage;
    auto session = Session::create(storage);
    const auto definition = context_document("1057.close-cancel");
    REQUIRE(set_document_value(*session, definition, 1.0, chord::background_context()).has_value());

    // The frozen gated storage: the caller is cancelled while `close()` runs.
    auto close_scope = chord::with_cancel(chord::background_context());
    storage.on_close([&close_scope](Storage&) { close_scope.cancel("stop close"); });
    const auto closing = consume(session->close(close_scope.context()));

    const auto frozen_error = frozen_abort_error(*scenario, "closeError");
    REQUIRE(frozen_error.has_value());
    REQUIRE_FALSE(closing.has_value());
    // Frozen `closeError`: AbortError / "The operation was aborted".
    CHECK(frozen_error->name == "AbortError");
    CHECK(closing.error().code == support::ErrorCode::Cancelled);
    CHECK(closing.error().message == frozen_error->message);
    CHECK(closing.error().detail == "stop close");

    // Frozen `storageCloseStarted` / `storageCloseCompleted`.
    const auto* frozen_started = scenario->member("storageCloseStarted");
    REQUIRE(frozen_started != nullptr);
    REQUIRE(frozen_started->holds<bool>());
    CHECK(frozen_started->get<bool>());
    CHECK(storage.close_calls() == 1);
    const auto* frozen_completed = scenario->member("storageCloseCompleted");
    REQUIRE(frozen_completed != nullptr);
    REQUIRE(frozen_completed->holds<bool>());
    CHECK(frozen_completed->get<bool>());

    // Frozen `sessionEnd: {reason: "closed"}`.
    const auto* frozen_end = scenario->object_member("sessionEnd");
    REQUIRE(frozen_end != nullptr);
    const auto frozen_reason = support::json_string_member(*frozen_end, "reason");
    REQUIRE(frozen_reason.has_value());
    CHECK(*frozen_reason == "closed");
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// Discriminating case for the close row: an already-cancelled caller still
// closes Storage and ends the Session. An implementation that checks the
// caller's context before starting mandatory cleanup skips the storage close
// and fails.
TEST_CASE("an already cancelled close caller still closes storage",
        "[durable][session][close][cancellation][issue1184][discriminating]") {
    tests::GatedStorage storage;
    auto session = Session::create(storage);
    auto scope = chord::with_cancel(chord::background_context());
    scope.cancel("stop close");

    const auto closing = consume(session->close(scope.context()));

    REQUIRE_FALSE(closing.has_value());
    CHECK(closing.error().code == support::ErrorCode::Cancelled);
    CHECK(closing.error().detail == "stop close");
    CHECK(storage.close_calls() == 1);
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// The composite capability's own declared comparison scenario: one real
// Session driven end to end with contexts — a value-carrying commit context
// reaching an observer without the committer's cancellation, a cancelled
// watch, and a cancelled close whose cleanup still completes.
TEST_CASE("C++ Session cancellation and close with a Context",
        "[durable][session][context][cancellation][issue1184][composite]") {
    tests::GatedStorage storage;
    auto session = Session::create(storage);
    const auto definition = context_document("1184.composite");
    const auto commit_key = chord::create_context_key<std::string>("1184-composite");

    std::stop_source producer;
    const auto producer_context = chord::with_abort_signal(producer.get_token(),
            "producer stopped",
            chord::with_context_value(commit_key, std::string{"commit-context"}, chord::background_context()));
    REQUIRE(set_document_value(*session, definition, 1.0, producer_context).has_value());
    producer.request_stop();

    auto watch_scope = chord::with_cancel(chord::background_context());
    auto acquired = consume(session->watch_doc(definition, DocumentAddress{}, watch_scope.context()));
    REQUIRE(acquired.has_value());
    REQUIRE(acquired.value().has_value());
    auto& watch = *acquired.value();

    std::vector<ObservedFrame> frames;
    REQUIRE(watch.start([&frames, &commit_key](DocumentFrame frame, chord::Context context) {
        frames.push_back(ObservedFrame{.value = std::move(frame.value),
                .abort_signal_present = context.stop_token().stop_possible(),
                .context_value = context.value(commit_key)});
    }));

    // The committer's cancellation is masked: the frame keeps the commit
    // context value and reports no abort signal.
    REQUIRE(set_document_value(*session, definition, 2.0, producer_context).has_value());
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].context_value.has_value());
    CHECK(*frames[0].context_value == "commit-context");
    CHECK_FALSE(frames[0].abort_signal_present);

    // Cancelling the watch's context ends delivery while the Session stays
    // usable and committed state advances.
    watch_scope.cancel("stop watching");
    REQUIRE(set_document_value(*session, definition, 3.0, chord::background_context()).has_value());
    CHECK(frames.size() == 1);
    CHECK(watch.stop() == WatchEndReason::Cancelled);
    auto snapshot = consume(session->snapshot(definition, DocumentAddress{}));
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().has_value());
    const auto snapshot_json = to_json(*snapshot.value());
    const auto* snapshot_object = support::json_object(snapshot_json);
    REQUIRE(snapshot_object != nullptr);
    const auto snapshot_value = support::json_number_member(*snapshot_object, "value");
    REQUIRE(snapshot_value.has_value());
    CHECK(*snapshot_value == 3.0);

    // The cancelled close still completes the mandatory cleanup.
    auto close_scope = chord::with_cancel(chord::background_context());
    storage.on_close([&close_scope](Storage&) { close_scope.cancel("stop close"); });
    const auto closing = consume(session->close(close_scope.context()));
    REQUIRE_FALSE(closing.has_value());
    CHECK(closing.error().code == support::ErrorCode::Cancelled);
    CHECK(storage.close_calls() == 1);
    CHECK(session->closed().reason == SessionEndReason::Closed);
}

// Discriminating case for the watch row's "a watch stop must not abort or join
// a running callback" half: a listener that stops or cancels its own watch
// while buffered frames are being delivered ends the batch. The callback that
// asked for the stop runs to completion, and the queued frames are discarded
// without reaching the listener.
TEST_CASE("stopping or cancelling a watch during a callback discards the queued frames",
        "[durable][session][watch][cancellation][issue1184][discriminating]") {
    MemoryStorage storage;
    auto session = Session::create(storage);
    const auto definition = context_document("1057.watch-self-stop");
    REQUIRE(set_document_value(*session, definition, 1.0, chord::background_context()).has_value());

    // Two frames are committed before either watch starts, so each delivery
    // batch holds a frame the callback can discard.
    auto stopped_scope = chord::with_cancel(chord::background_context());
    auto stopped_watch = consume(session->watch_doc(definition, DocumentAddress{}, stopped_scope.context()));
    REQUIRE(stopped_watch.has_value());
    REQUIRE(stopped_watch.value().has_value());

    auto cancelled_scope = chord::with_cancel(chord::background_context());
    auto cancelled_watch = consume(session->watch_doc(definition, DocumentAddress{}, cancelled_scope.context()));
    REQUIRE(cancelled_watch.has_value());
    REQUIRE(cancelled_watch.value().has_value());

    REQUIRE(set_document_value(*session, definition, 2.0, chord::background_context()).has_value());
    REQUIRE(set_document_value(*session, definition, 3.0, chord::background_context()).has_value());

    // The first callback stops its own watch; the second cancels its own
    // acquisition context.
    std::vector<ObservedFrame> stopped_frames;
    bool stopped_callback_completed = false;
    // A terminal reason this case never produces, so the check below only passes
    // when the callback's own `stop()` produced it.
    WatchEndReason stopped_during_callback{WatchEndReason::SessionClosed};
    REQUIRE(stopped_watch.value().value().start([&](DocumentFrame frame, chord::Context) {
        stopped_frames.push_back(
                ObservedFrame{.value = std::move(frame.value), .abort_signal_present = false, .context_value = {}});
        stopped_during_callback = stopped_watch.value().value().stop();
        stopped_callback_completed = true;
    }));

    std::vector<ObservedFrame> cancelled_frames;
    bool cancelled_callback_completed = false;
    REQUIRE(cancelled_watch.value().value().start([&](DocumentFrame frame, chord::Context) {
        cancelled_frames.push_back(
                ObservedFrame{.value = std::move(frame.value), .abort_signal_present = false, .context_value = {}});
        cancelled_scope.cancel("stop watching");
        cancelled_callback_completed = true;
    }));

    // The running callback completed and delivered exactly its own frame; the
    // queued frame was discarded rather than delivered after the stop.
    CHECK(stopped_callback_completed);
    CHECK(stopped_during_callback == WatchEndReason::Stopped);
    CHECK(stopped_frames.size() == 1);
    CHECK(cancelled_callback_completed);
    CHECK(cancelled_frames.size() == 1);
    CHECK(stopped_watch.value().value().stop() == WatchEndReason::Stopped);
    CHECK(cancelled_watch.value().value().stop() == WatchEndReason::Cancelled);
}
