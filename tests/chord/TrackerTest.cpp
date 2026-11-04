#include <cch/chord/Delta.hpp>

#include <cch/support/JsonAccess.hpp>

#include "support/ChordEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
using namespace cch::chord;

namespace {

/// The frozen #1057 delta-revisions observation the receipts register.
[[nodiscard]] cch::tests::FrozenChordScenario delta_revisions_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1057", "delta-revisions.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen #1061 prepare-replace observation.
[[nodiscard]] cch::tests::FrozenChordScenario prepare_replace_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1061", "prepare-replace.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// Decode one frozen operation tuple array (the artifact's own `ops` field)
/// into the typed operation batch.
[[nodiscard]] OpBatch decode_frozen_ops(const support::JsonValue& frozen_tuples) {
    auto json = Json::from_support(frozen_tuples);
    REQUIRE(json.has_value());
    auto decoded = decode_ops(*json);
    REQUIRE(decoded.has_value());
    return *decoded;
}

/// The frozen `expected` member with this name, decoded as an operation batch.
[[nodiscard]] OpBatch frozen_expected_ops(const cch::tests::FrozenChordScenario& frozen, std::string_view name) {
    const auto* tuples = frozen.array_member(name);
    REQUIRE(tuples != nullptr);
    return decode_frozen_ops(support::JsonValue{*tuples});
}

/// The frozen `inputs` member with this name as an immutable Chord value.
[[nodiscard]] Json frozen_input_json(const cch::tests::FrozenChordScenario& frozen, std::string_view name) {
    const auto* value = frozen.inputs_member(name);
    REQUIRE(value != nullptr);
    auto json = Json::from_support(*value);
    REQUIRE(json.has_value());
    return *json;
}

/// The frozen `expected` member with this name as an immutable Chord value.
[[nodiscard]] Json frozen_expected_json(const cch::tests::FrozenChordScenario& frozen, std::string_view name) {
    const auto* value = frozen.member(name);
    REQUIRE(value != nullptr);
    auto json = Json::from_support(*value);
    REQUIRE(json.has_value());
    return *json;
}

/// Compare one frozen `{name, message}` error field with the observed error.
void check_frozen_error(const support::JsonValue::object_t& frozen, const support::Error& error) {
    const auto message = support::json_string_member(frozen, "message");
    REQUIRE(message.has_value());
    CHECK(error.message == *message);
}

[[nodiscard]] Path keys(std::initializer_list<const char*> parts) {
    Path path;
    path.reserve(parts.size());
    for (const char* part : parts)
        path.push_back(path_key(part));
    return path;
}

[[nodiscard]] Json object(std::initializer_list<Json::Object::value_type> entries) {
    return Json{Json::Object{entries}};
}

[[nodiscard]] Json array(std::initializer_list<Json> entries) { return Json{Json::Array{entries}}; }

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "prepared revisions are immutable and a same-base preparation becomes stale after adoption".
// The frozen initial value and first/second writes drive the tracker and the frozen before,
// firstOps, revisionAfterFirst, afterFirst, staleError, valueAfterStale and revisionAfterStale
// fields are compared exactly.
TEST_CASE("a same-base preparation becomes stale after another preparation is adopted",
        "[chord][delta][revision][stale][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario(
            "prepared revisions are immutable and a same-base preparation becomes stale after adoption");
    const auto first_value = frozen.inputs_number("firstValue");
    const auto second_value = frozen.inputs_number("secondValue");
    REQUIRE(first_value.has_value());
    REQUIRE(second_value.has_value());
    const auto* frozen_stale_error = frozen.object_member("staleError");
    REQUIRE(frozen_stale_error != nullptr);

    auto tracker = track(frozen_input_json(frozen, "initial"));
    CHECK(tracker.value() == frozen_expected_json(frozen, "before"));

    auto first = tracker.begin_change();
    REQUIRE(first.draft().set(keys({"value"}), Json{*first_value}));
    auto first_prepared = first.prepare();
    REQUIRE(first_prepared);
    CHECK(first_prepared->ops() == frozen_expected_ops(frozen, "firstOps"));
    REQUIRE(first_prepared->ops().size() == 1);
    CHECK(std::holds_alternative<SetOp>(first_prepared->ops().front()));

    auto second = tracker.begin_change();
    REQUIRE(second.draft().set(keys({"value"}), Json{*second_value}));
    auto second_prepared = second.prepare();
    REQUIRE(second_prepared);

    REQUIRE(tracker.adopt(*first_prepared));
    const auto revision_after_first = frozen.number_member("revisionAfterFirst");
    REQUIRE(revision_after_first.has_value());
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision_after_first));
    CHECK(tracker.value() == frozen_expected_json(frozen, "afterFirst"));
    CHECK(frozen_expected_json(frozen, "before") == frozen_input_json(frozen, "initial"));

    const auto stale = tracker.adopt(*second_prepared);
    REQUIRE_FALSE(stale);
    check_frozen_error(*frozen_stale_error, stale.error());
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision_after_first));
    CHECK(tracker.value() == frozen_expected_json(frozen, "valueAfterStale"));
    const auto revision_after_stale = frozen.number_member("revisionAfterStale");
    REQUIRE(revision_after_stale.has_value());
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision_after_stale));

    const auto used = tracker.adopt(*first_prepared);
    REQUIRE_FALSE(used);
    CHECK(used.error().message == "Prepared change has already been used");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "aborting a changed draft revokes escaped handles and leaves the committed revision unchanged".
// The frozen initial, drafted and escaped values drive the change, and the frozen escapedError,
// adoptError, value and revision fields are compared exactly.
TEST_CASE("aborting a prepared change revokes escaped draft handles",
        "[chord][delta][revision][abort][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario(
            "aborting a changed draft revokes escaped handles and leaves the committed revision unchanged");
    const auto drafted_value = frozen.inputs_number("draftedValue");
    const auto escaped_write = frozen.inputs_number("escapedWrite");
    REQUIRE(drafted_value.has_value());
    REQUIRE(escaped_write.has_value());
    const auto* frozen_escaped_error = frozen.object_member("escapedError");
    const auto* frozen_adopt_error = frozen.object_member("adoptError");
    REQUIRE(frozen_escaped_error != nullptr);
    REQUIRE(frozen_adopt_error != nullptr);

    auto tracker = track(frozen_input_json(frozen, "initial"));
    auto change = tracker.begin_change();
    auto escaped = change.draft();
    REQUIRE(escaped.set(keys({"value"}), Json{*drafted_value}));
    auto prepared = change.prepare();
    REQUIRE(prepared);

    change.abort();
    const auto escaped_write_result = escaped.set(keys({"value"}), Json{*escaped_write});
    REQUIRE_FALSE(escaped_write_result);
    check_frozen_error(*frozen_escaped_error, escaped_write_result.error());
    const auto escaped_read = escaped.get(keys({"value"}));
    REQUIRE_FALSE(escaped_read);
    check_frozen_error(*frozen_escaped_error, escaped_read.error());

    const auto abandoned = tracker.adopt(*prepared);
    REQUIRE_FALSE(abandoned);
    check_frozen_error(*frozen_adopt_error, abandoned.error());
    CHECK(tracker.value() == frozen_expected_json(frozen, "value"));
    const auto revision = frozen.number_member("revision");
    REQUIRE(revision.has_value());
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision));

    const auto settled = change.prepare();
    REQUIRE_FALSE(settled);
    CHECK(settled.error().message == "Change has already been settled");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "a structural no-op emits a nonempty batch while an untouched change emits none and still adopts".
TEST_CASE("a structural no-op emits splice operations while an untouched change emits none",
        "[chord][delta][revision][noop][issue1061][spec]") {
    auto tracker = track(object({{"items", array({Json{"a"}})}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.splice(keys({"items"}), 0, 1, {}));
    REQUIRE(draft.splice(keys({"items"}), 0, 0, {Json{"a"}}));
    auto prepared = change.prepare();
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 2);
    const auto* remove = std::get_if<SpliceOp>(&prepared->ops()[0]);
    const auto* insert = std::get_if<SpliceOp>(&prepared->ops()[1]);
    REQUIRE(remove != nullptr);
    REQUIRE(insert != nullptr);
    CHECK(remove->index == 0);
    CHECK(remove->remove == 1);
    CHECK(insert->index == 0);
    CHECK(insert->remove == 0);
    REQUIRE(insert->items.size() == 1);
    CHECK(insert->items[0] == Json{"a"});
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"items", array({Json{"a"}})}}));

    auto empty_tracker = track(object({{"value", Json{1}}}));
    auto empty_change = empty_tracker.begin_change();
    auto empty_prepared = empty_change.prepare();
    REQUIRE(empty_prepared);
    CHECK(empty_prepared->ops().empty());
    CHECK(empty_prepared->value().shares_container_with(empty_tracker.value()));
    REQUIRE(empty_tracker.adopt(*empty_prepared));
    CHECK(empty_tracker.revision() == 1);
    CHECK(empty_tracker.value() == object({{"value", Json{1}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/prepare-replace.json
// "a prepared whole-root replacement keeps the prior revision as its base and stales a same-base
// preparation after adoption". The frozen initial, replacement and competing draft value drive
// the tracker; the frozen preparedBase/preparedValue/ops/baseRevision/revisionBefore/baseSame/
// ownsReplacement/revisionAfter/valueAfter/preparedBaseAfter/staleError/usedError fields are
// compared exactly.
TEST_CASE("prepare replace keeps the checkpoint base immutable and stales same-base preparations",
        "[chord][delta][revision][prepare-replace][issue1061][spec]") {
    const auto frozen = prepare_replace_scenario(
            "a prepared whole-root replacement keeps the prior revision as its base and stales a same-base preparation "
            "after adoption");
    const auto competing_value = frozen.inputs_number("competingDraftValue");
    REQUIRE(competing_value.has_value());
    const auto* frozen_ops = frozen.array_member("ops");
    const auto* frozen_stale_error = frozen.object_member("staleError");
    const auto* frozen_used_error = frozen.object_member("usedError");
    REQUIRE(frozen_ops != nullptr);
    REQUIRE(frozen_stale_error != nullptr);
    REQUIRE(frozen_used_error != nullptr);

    auto tracker = track(frozen_input_json(frozen, "initial"));
    const Json snapshot = tracker.value();

    auto prepared = tracker.prepare_replace(frozen_input_json(frozen, "replacement"));
    REQUIRE(prepared);
    CHECK(prepared->value() == frozen_expected_json(frozen, "preparedValue"));
    CHECK(prepared->base() == frozen_expected_json(frozen, "preparedBase"));
    CHECK(prepared->ops() == frozen_expected_ops(frozen, "ops"));
    REQUIRE(prepared->ops().size() == 1);
    const auto* replace_op = std::get_if<ReplaceOp>(&prepared->ops().front());
    REQUIRE(replace_op != nullptr);
    CHECK(replace_op->value == prepared->value());
    const auto base_revision = frozen.number_member("baseRevision");
    REQUIRE(base_revision.has_value());
    CHECK(prepared->base_revision() == static_cast<std::uint64_t>(*base_revision));
    const auto base_same = frozen.boolean_member("baseSame");
    const auto owns_replacement = frozen.boolean_member("ownsReplacement");
    const auto revision_before = frozen.number_member("revisionBefore");
    REQUIRE(base_same.has_value());
    REQUIRE(owns_replacement.has_value());
    REQUIRE(revision_before.has_value());
    CHECK(prepared->base().shares_container_with(tracker.value()) == *base_same);
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision_before));
    CHECK(tracker.value() == frozen_input_json(frozen, "initial"));

    auto competitor = tracker.begin_change();
    REQUIRE(competitor.draft().set(keys({"value"}), Json{*competing_value}));
    auto competitor_prepared = competitor.prepare();
    REQUIRE(competitor_prepared);

    REQUIRE(tracker.adopt(*prepared));
    const auto revision_after = frozen.number_member("revisionAfter");
    REQUIRE(revision_after.has_value());
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision_after));
    CHECK(tracker.value() == frozen_expected_json(frozen, "valueAfter"));
    CHECK(snapshot == frozen_input_json(frozen, "initial"));
    CHECK(prepared->base() == frozen_expected_json(frozen, "preparedBaseAfter"));

    const auto stale = tracker.adopt(*competitor_prepared);
    REQUIRE_FALSE(stale);
    check_frozen_error(*frozen_stale_error, stale.error());
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision_after));
    CHECK(tracker.value() == frozen_expected_json(frozen, "valueAfter"));

    const auto used = tracker.adopt(*prepared);
    REQUIRE_FALSE(used);
    check_frozen_error(*frozen_used_error, used.error());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/prepare-replace.json
// "a deeply equal whole-root replacement emits no operations and still stales a same-base
// preparation": the frozen initial and replacement drive the tracker and the frozen ops/valueSame/
// baseSame/ownsReplacement/revision/value/staleError fields are compared exactly.
TEST_CASE("prepare replace of a deeply equal root emits no operations and stales a competitor",
        "[chord][delta][revision][prepare-replace][noop][issue1061][spec]") {
    const auto frozen = prepare_replace_scenario(
            "a deeply equal whole-root replacement emits no operations and still stales a same-base preparation");
    const auto* frozen_stale_error = frozen.object_member("staleError");
    REQUIRE(frozen_stale_error != nullptr);
    const auto value_same = frozen.boolean_member("valueSame");
    const auto base_same = frozen.boolean_member("baseSame");
    const auto owns_replacement = frozen.boolean_member("ownsReplacement");
    const auto revision = frozen.number_member("revision");
    REQUIRE(value_same.has_value());
    REQUIRE(base_same.has_value());
    REQUIRE(owns_replacement.has_value());
    REQUIRE(revision.has_value());

    auto tracker = track(frozen_input_json(frozen, "initial"));
    auto prepared = tracker.prepare_replace(frozen_input_json(frozen, "replacement"));
    auto competitor = tracker.prepare_replace(frozen_input_json(frozen, "replacement"));
    REQUIRE(prepared);
    REQUIRE(competitor);
    const auto frozen_ops = frozen_expected_ops(frozen, "ops");
    CHECK(frozen_ops.empty());
    CHECK(prepared->ops() == frozen_ops);
    CHECK(prepared->value() == tracker.value());
    CHECK(prepared->base() == tracker.value());
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision));
    CHECK(tracker.value() == frozen_expected_json(frozen, "value"));
    const auto stale = tracker.adopt(*competitor);
    REQUIRE_FALSE(stale);
    check_frozen_error(*frozen_stale_error, stale.error());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/prepare-replace.json
// "an aborted whole-root preparation cannot be adopted": the frozen initial and replacement drive
// the tracker and the frozen abortedError/revision/value fields are compared exactly.
TEST_CASE("an aborted or foreign whole-root preparation cannot be adopted",
        "[chord][delta][revision][prepare-replace][abort][issue1061][spec]") {
    const auto frozen = prepare_replace_scenario("an aborted whole-root preparation cannot be adopted");
    const auto* frozen_aborted_error = frozen.object_member("abortedError");
    REQUIRE(frozen_aborted_error != nullptr);
    const auto revision = frozen.number_member("revision");
    REQUIRE(revision.has_value());

    auto tracker = track(frozen_input_json(frozen, "initial"));
    auto prepared = tracker.prepare_replace(frozen_input_json(frozen, "replacement"));
    REQUIRE(prepared);
    prepared->abort();
    const auto aborted = tracker.adopt(*prepared);
    REQUIRE_FALSE(aborted);
    check_frozen_error(*frozen_aborted_error, aborted.error());
    CHECK(tracker.value() == frozen_expected_json(frozen, "value"));
    CHECK(tracker.revision() == static_cast<std::uint64_t>(*revision));

    auto other = track(object({{"value", Json{10}}}));
    auto foreign = other.prepare_replace(object({{"value", Json{11}}}));
    REQUIRE(foreign);
    const auto rejected = tracker.adopt(*foreign);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "Prepared change belongs to a different tracker");
    CHECK(tracker.value() == frozen_expected_json(frozen, "value"));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/draft-array-holes.json
// "deleting an array element from a draft rejects the hole and leaves the prepared revision empty".
TEST_CASE("draft paths reject unsafe empty and out-of-range mutations before recording",
        "[chord][delta][revision][validation][issue1061][spec]") {
    auto tracker = track(object({{"items", array({Json{"a"}})}, {"value", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    const auto unsafe = draft.set(Path{path_key("__proto__")}, Json{true});
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().message == "unsafe path segment: __proto__");

    const auto empty = draft.set(Path{}, Json{true});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().message == "path is empty");

    const auto out_of_range = draft.erase(Path{path_key("items"), path_index(2)});
    REQUIRE_FALSE(out_of_range);
    CHECK(out_of_range.error().message == "Overlay arrays cannot contain holes");

    const auto array_delete = draft.erase(Path{path_key("items"), path_index(0)});
    REQUIRE_FALSE(array_delete);
    CHECK(array_delete.error().message == "Overlay arrays cannot contain holes");

    const auto bad_permutation = draft.move(keys({"items"}), {1, 1});
    REQUIRE_FALSE(bad_permutation);
    CHECK(bad_permutation.error().message == "m permutation is not a bijection");

    auto prepared = change.prepare();
    REQUIRE(prepared);
    CHECK(prepared->ops().empty());
    CHECK(prepared->value() == tracker.value());
}

TEST_CASE("draft reads see writes while the committed revision stays unchanged",
        "[chord][delta][revision][draft][issue1061][spec]") {
    auto tracker = track(object({{"value", Json{1}}, {"text", Json{"abc"}}}));
    const Json snapshot = tracker.value();
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.set(keys({"value"}), Json{2}));
    REQUIRE(draft.append_string(keys({"text"}), "d"));

    auto read = draft.get(keys({"value"}));
    REQUIRE(read);
    REQUIRE(read->has_value());
    CHECK(**read == Json{2});
    auto text = draft.get(keys({"text"}));
    REQUIRE(text);
    REQUIRE(text->has_value());
    CHECK(text->value().as_string() != nullptr);
    CHECK(*text->value().as_string() == "abcd");
    CHECK(tracker.value() == snapshot);
    CHECK(tracker.revision() == 0);

    const auto duplicate = draft.set(keys({"value"}), Json{2});
    REQUIRE(duplicate);
    auto prepared = change.prepare();
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 2);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/review-fixes.json
// "draft reads distinguish absent leaves from null and from absent receiver paths".
TEST_CASE("draft reads distinguish absent leaves nulls and invalid traversal",
        "[chord][delta][revision][draft][read][issue1061][spec]") {
    auto tracker = track(object({
            {"present", Json{1}},
            {"nil", Json{nullptr}},
            {"object", object({{"present", Json{2}}})},
            {"items", array({Json{10}})},
    }));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    auto present = draft.get(keys({"present"}));
    REQUIRE(present);
    REQUIRE(present->has_value());
    CHECK(**present == Json{1});

    auto missing_leaf = draft.get(keys({"missing"}));
    REQUIRE(missing_leaf);
    CHECK_FALSE(missing_leaf->has_value());

    auto null_leaf = draft.get(keys({"nil"}));
    REQUIRE(null_leaf);
    REQUIRE(null_leaf->has_value());
    CHECK(**null_leaf == Json{nullptr});

    auto nested_missing_leaf = draft.get(Path{path_key("object"), path_key("missing")});
    REQUIRE(nested_missing_leaf);
    CHECK_FALSE(nested_missing_leaf->has_value());

    auto missing_array_index = draft.get(Path{path_key("items"), path_index(2)});
    REQUIRE(missing_array_index);
    CHECK_FALSE(missing_array_index->has_value());

    const auto missing_receiver = draft.get(Path{path_key("missing"), path_key("deep")});
    REQUIRE_FALSE(missing_receiver);
    CHECK(missing_receiver.error().message.starts_with("unresolvable path"));

    const auto null_receiver = draft.get(Path{path_key("nil"), path_key("deep")});
    REQUIRE_FALSE(null_receiver);
    CHECK(null_receiver.error().message.starts_with("unresolvable path"));

    auto prepared = change.prepare();
    REQUIRE(prepared);
    const auto settled = draft.get(keys({"missing"}));
    REQUIRE_FALSE(settled);
    CHECK(settled.error().message == "Cannot use a settled overlay");
}

// The frozen prepared-state errors of fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// ("aborting a changed draft revokes escaped handles and leaves the committed revision unchanged"
// and "prepared revisions are immutable and a same-base preparation becomes stale after
// adoption") are the contract for the revoked open draft, the stale prepared competitor and the
// aborted prepared replacement below.
TEST_CASE("adoption immediately revokes escaped competing draft handles",
        "[chord][delta][revision][adopt][draft][issue1061][spec]") {
    const auto frozen_abort = delta_revisions_scenario(
            "aborting a changed draft revokes escaped handles and leaves the committed revision unchanged");
    const auto frozen_stale = delta_revisions_scenario(
            "prepared revisions are immutable and a same-base preparation becomes stale after adoption");
    const auto* frozen_aborted_error = frozen_abort.object_member("adoptError");
    const auto* frozen_stale_error = frozen_stale.object_member("staleError");
    REQUIRE(frozen_aborted_error != nullptr);
    REQUIRE(frozen_stale_error != nullptr);

    auto tracker = track(object({{"x", Json{1}}}));
    auto open = tracker.begin_change();
    auto open_draft = open.draft();
    REQUIRE(open_draft.set(keys({"x"}), Json{5}));

    auto prepared_competitor = tracker.begin_change();
    REQUIRE(prepared_competitor.draft().set(keys({"x"}), Json{6}));
    auto prepared = prepared_competitor.prepare();
    REQUIRE(prepared);
    auto aborted_competitor = tracker.prepare_replace(object({{"x", Json{3}}}));
    REQUIRE(aborted_competitor);

    auto winner = tracker.prepare_replace(object({{"x", Json{2}}}));
    REQUIRE(winner);
    REQUIRE(tracker.adopt(*winner));
    CHECK(tracker.revision() == 1);

    const auto open_prepare = open.prepare();
    REQUIRE_FALSE(open_prepare);
    CHECK(open_prepare.error().message == "Cannot use a settled overlay");

    const auto open_read = open_draft.get(keys({"x"}));
    REQUIRE_FALSE(open_read);
    CHECK(open_read.error().message == "Cannot use a settled overlay");

    const auto open_write = open_draft.set(keys({"x"}), Json{7});
    REQUIRE_FALSE(open_write);
    CHECK(open_write.error().message == "Cannot use a settled overlay");

    open.abort();
    const auto stale = tracker.adopt(*prepared);
    REQUIRE_FALSE(stale);
    check_frozen_error(*frozen_stale_error, stale.error());

    aborted_competitor->abort();
    const auto aborted = tracker.adopt(*aborted_competitor);
    REQUIRE_FALSE(aborted);
    check_frozen_error(*frozen_aborted_error, aborted.error());
    CHECK(tracker.value() == object({{"x", Json{2}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/review-fixes.json
// "adding a key and erasing it again normalizes to an empty batch over the unchanged base".
TEST_CASE("erasing a newly added key prepares an empty batch over the unchanged base",
        "[chord][delta][revision][ordering][normalize][issue1061][spec]") {
    auto tracker = track(object({{"a", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.set(keys({"added"}), object({{"leaf", Json{3}}})));
    REQUIRE(draft.erase(Path{path_key("added"), path_key("leaf")}));
    REQUIRE(draft.erase(keys({"added"})));

    auto prepared = change.prepare();
    REQUIRE(prepared);
    CHECK(prepared->ops().empty());
    CHECK(prepared->value().shares_container_with(tracker.value()));
    CHECK(prepared->value() == object({{"a", Json{1}}}));
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"a", Json{1}}}));
}

// Frozen observation: packages/chord/src/delta/tracker.ts:emitObjectOperations
// encodes delete-and-readd explicitly; a later parent write supersedes its
// descendants and a repeated leaf write emits only the final value. The frozen
// vocabulary record (fixtures/pi-v1.3.0/audit-1057/delta-revisions.json) supplies the
// delete-case object, its recorded delete operation and its adopted value, and the set-case
// single-set shape the folded write must match.
TEST_CASE("draft writes fold to the final leaf while delete and readd stays explicit",
        "[chord][delta][revision][ordering][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario(
            "the tracker emits the set, append, splice, permutation, delete, truncate, and replace vocabulary");
    const auto* delete_case = frozen.object_member("delete");
    const auto* set_case = frozen.object_member("set");
    REQUIRE(delete_case != nullptr);
    REQUIRE(set_case != nullptr);
    const auto* delete_ops = support::json_array_member(*delete_case, "ops");
    REQUIRE(delete_ops != nullptr);
    const auto* frozen_cases = frozen.inputs_member("cases");
    REQUIRE(frozen_cases != nullptr);
    const auto* frozen_cases_object = frozen_cases->get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_cases_object != nullptr);
    const auto* delete_input = support::json_object_member(*frozen_cases_object, "delete");
    REQUIRE(delete_input != nullptr);
    auto delete_input_json = Json::from_support(*delete_input);
    REQUIRE(delete_input_json.has_value());

    auto tracker = track(*delete_input_json);
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.erase(Path{path_key("o"), path_key("b")}));
    REQUIRE(draft.set(Path{path_key("o"), path_key("b")}, Json{2}));
    auto prepared = change.prepare();
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 2);
    // Delete-and-readd stays explicit: the first prepared operation is the frozen delete
    // case's own recorded operation.
    const auto frozen_delete_ops = decode_frozen_ops(support::JsonValue{*delete_ops});
    REQUIRE(frozen_delete_ops.size() == 1);
    CHECK(prepared->ops()[0] == frozen_delete_ops[0]);
    const auto* deletion = std::get_if<DeleteOp>(&prepared->ops()[0]);
    const auto* set = std::get_if<SetOp>(&prepared->ops()[1]);
    REQUIRE(deletion != nullptr);
    REQUIRE(set != nullptr);
    CHECK(deletion->path == Path{path_key("o"), path_key("b")});
    CHECK(set->path == Path{path_key("o"), path_key("b")});
    CHECK(set->value == Json{2});
    REQUIRE(tracker.adopt(*prepared));
    // The frozen delete case's recorded value covers the pure delete; this delete-and-readd
    // variant re-adds the erased member, so the adopted value is the frozen input object with
    // the member restored.
    CHECK(tracker.value() == object({{"o", object({{"a", Json{1}}, {"b", Json{2}}})}}));

    // A repeated leaf write folds to the final value: exactly one set operation in the frozen
    // set case's recorded shape.
    const auto* set_ops = support::json_array_member(*set_case, "ops");
    REQUIRE(set_ops != nullptr);
    const auto frozen_set_ops = decode_frozen_ops(support::JsonValue{*set_ops});
    REQUIRE(frozen_set_ops.size() == 1);
    auto fold_tracker = track(object({{"o", object({{"a", Json{1}}})}}));
    auto fold_change = fold_tracker.begin_change();
    auto fold_draft = fold_change.draft();
    REQUIRE(fold_draft.set(Path{path_key("o"), path_key("a")}, Json{1}));
    REQUIRE(fold_draft.set(Path{path_key("o"), path_key("a")}, Json{2}));
    auto fold_prepared = fold_change.prepare();
    REQUIRE(fold_prepared);
    REQUIRE(fold_prepared->ops().size() == 1);
    const auto* fold_set = std::get_if<SetOp>(&fold_prepared->ops()[0]);
    REQUIRE(fold_set != nullptr);
    CHECK(std::holds_alternative<SetOp>(frozen_set_ops[0]));
    CHECK(fold_set->value == Json{2});
}
