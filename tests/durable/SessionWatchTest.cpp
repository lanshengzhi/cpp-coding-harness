#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/JsonAccess.hpp>

#include "durable/FrozenExpectation.hpp"
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
    REQUIRE(watch.value().value().start([&frames](DocumentFrame frame) { frames.push_back(std::move(frame)); }));

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
            [&deliveries](DocumentFrame frame) { deliveries.push_back(std::move(frame)); }));

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
