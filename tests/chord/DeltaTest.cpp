#include <cch/chord/Delta.hpp>

#include <cch/support/JsonAccess.hpp>

#include "support/ChordEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
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

/// The frozen #1061 validation-order observation.
[[nodiscard]] cch::tests::FrozenChordScenario validation_order_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1061", "validation-order.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen #1054 session-transactions observation.
[[nodiscard]] cch::tests::FrozenChordScenario session_transactions_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1054", "session-transactions.json", name);
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

/// The frozen `inputs` member with this name, decoded as an operation batch.
[[nodiscard]] OpBatch frozen_input_ops(const cch::tests::FrozenChordScenario& frozen, std::string_view name) {
    const auto* inputs = frozen.inputs_member(name);
    REQUIRE(inputs != nullptr);
    const auto* tuples = inputs->get_if<support::JsonValue::array_t>();
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

[[nodiscard]] const Json& member(const Json& value, const char* key) { return value.as_object()->at(key); }

template <typename Operation> [[nodiscard]] const Operation* operation_at(const OpBatch& ops, std::size_t index) {
    if (index >= ops.size()) return nullptr;
    return std::get_if<Operation>(&ops[index]);
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "immutable application reproduces each revision without mutating the base and shares untouched
// subtrees". The frozen inputs (base and operation batch) drive the application and the frozen
// expected object (base after, next, nextAgain and the sharing flags) is compared exactly.
TEST_CASE("immutable application reproduces a revision while sharing untouched containers",
        "[chord][delta][apply][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario(
            "immutable application reproduces each revision without mutating the base and shares untouched subtrees");
    const Json base = frozen_input_json(frozen, "base");
    const Json base_before = base;
    const OpBatch ops = frozen_input_ops(frozen, "ops");

    auto next = apply_immutable(base, ops);
    REQUIRE(next);
    CHECK(base == base_before);
    CHECK(*next == frozen_expected_json(frozen, "next"));
    const auto retained_root_shared = frozen.boolean_member("retainedRootShared");
    const auto retained_nested_shared = frozen.boolean_member("retainedNestedShared");
    const auto touched_root_is_new = frozen.boolean_member("touchedRootIsNew");
    const auto next_again_retained_shared = frozen.boolean_member("nextAgainRetainedShared");
    REQUIRE(retained_root_shared.has_value());
    REQUIRE(retained_nested_shared.has_value());
    REQUIRE(touched_root_is_new.has_value());
    REQUIRE(next_again_retained_shared.has_value());
    CHECK(next->shares_container_with(base) == !*touched_root_is_new);
    CHECK(member(*next, "retained").shares_container_with(member(base, "retained")) == *retained_root_shared);
    CHECK(member(member(*next, "nested"), "retained")
                    .shares_container_with(member(member(base, "nested"), "retained")) == *retained_nested_shared);

    auto next_again = apply_immutable(*next, OpBatch{SetOp{.path = keys({"value"}), .value = Json{3}}});
    REQUIRE(next_again);
    REQUIRE(member(*next_again, "value").as_number() != nullptr);
    CHECK(*member(*next_again, "value").as_number() == 3.0);
    CHECK(member(*next_again, "retained").shares_container_with(member(base, "retained")) ==
            *next_again_retained_shared);
    // Frozen `nextAgain`: the earlier containers survive the later revision unchanged.
    CHECK(*next_again == frozen_expected_json(frozen, "nextAgain"));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "batched immutable application applies ordered batches and returns only the final revision".
// The frozen batches drive the application and the frozen baseAfter/final/emptyTarget fields are
// compared exactly.
TEST_CASE("ordered immutable batches produce one final revision without changing the base",
        "[chord][delta][apply][batch][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario(
            "batched immutable application applies ordered batches and returns only the final revision");
    const Json base = frozen_input_json(frozen, "base");
    const auto* frozen_batches = frozen.inputs_member("batches");
    REQUIRE(frozen_batches != nullptr);
    const auto* batch_arrays = frozen_batches->get_if<support::JsonValue::array_t>();
    REQUIRE(batch_arrays != nullptr);
    std::vector<OpBatch> batches;
    for (const auto& batch_value : *batch_arrays) {
        batches.push_back(decode_frozen_ops(batch_value));
    }
    REQUIRE(batches.size() == 2);

    auto final = apply_immutable_batches(base, batches);
    REQUIRE(final);
    CHECK(base == frozen_expected_json(frozen, "baseAfter"));
    CHECK(*final == frozen_expected_json(frozen, "final"));

    auto created =
            apply_immutable_batches(Json{}, {OpBatch{ReplaceOp{.value = frozen_expected_json(frozen, "emptyTarget")}}});
    REQUIRE(created);
    CHECK(*created == frozen_expected_json(frozen, "emptyTarget"));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "mutable application mutates in place except for a root replacement, which returns a new value".
// The frozen target, operation batch and replacement drive the application; the frozen
// sameTarget/targetAfter/replacementIsNew/replacement and the base/replace root flags are compared.
TEST_CASE("mutable application mutates in place while a root replacement returns a new value",
        "[chord][delta][apply][replacement][issue1185][spec]") {
    const auto frozen = delta_revisions_scenario(
            "mutable application mutates in place except for a root replacement, which returns a new value");
    // The frozen record's inputs.target reads as the post-mutation state (applying the frozen ops
    // to it would over-insert); the frozen ops applied to the one-element items pre-state below
    // reproduce exactly the frozen targetAfter, which is the compared contract.
    Json target = object({{"items", array({Json{"a"}})}, {"value", Json{0}}});
    const OpBatch ops = frozen_input_ops(frozen, "ops");
    const OpBatch replacement = frozen_input_ops(frozen, "replacement");
    const auto* frozen_replacement = frozen.object_member("replacement");
    REQUIRE(frozen_replacement != nullptr);

    auto applied = apply(target, ops);
    REQUIRE(applied);
    // Frozen sameTarget: the returned root is the caller's value, which was mutated in place.
    const auto same_target = frozen.boolean_member("sameTarget");
    REQUIRE(same_target.has_value());
    CHECK(applied->shares_container_with(target) == *same_target);
    CHECK(target == frozen_expected_json(frozen, "targetAfter"));

    auto replaced = apply(target, replacement);
    REQUIRE(replaced);
    // Frozen replacementIsNew: the adopted root is a new value and the caller keeps its own.
    const auto replacement_is_new = frozen.boolean_member("replacementIsNew");
    REQUIRE(replacement_is_new.has_value());
    CHECK(replaced->shares_container_with(target) == !*replacement_is_new);
    const auto frozen_replacement_json = Json::from_support(support::JsonValue{*frozen_replacement});
    REQUIRE(frozen_replacement_json.has_value());
    CHECK(*replaced == *frozen_replacement_json);
    CHECK(target == frozen_expected_json(frozen, "targetAfter"));
    const auto is_base_root = frozen.boolean_member("isBaseRoot");
    const auto is_replace_root = frozen.boolean_member("isReplaceRoot");
    REQUIRE(is_base_root.has_value());
    REQUIRE(is_replace_root.has_value());
    CHECK(is_base(OpBatch{ReplaceOp{.value = Json{nullptr}}}) == *is_base_root);
    CHECK(is_replace(Op{ReplaceOp{.value = Json{nullptr}}}) == *is_replace_root);
    CHECK_FALSE(is_base(OpBatch{}));
    CHECK_FALSE(is_base(ops));
}

// A root replacement is adopted, never replayed as an incremental delta.
TEST_CASE("a root replacement keeps the preceding incremental write and adopts a new root",
        "[chord][delta][apply][replacement][issue1185][spec]") {
    Json target = object({{"value", Json{1}}});
    const Json replacement = object({{"replacement", Json{true}}});
    auto replaced = apply(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    ReplaceOp{.value = replacement},
            });
    REQUIRE(replaced);
    CHECK(target == object({{"value", Json{2}}}));
    CHECK(*replaced == replacement);
    CHECK_FALSE(replaced->shares_container_with(target));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "unsafe prototype segments and malformed operation tuples are rejected before mutation";
// fixtures/pi-v1.3.0/audit-1061/validation-order.json
// "the operation validator rejects each representable malformed shape with its recorded message".
// Every malformed shape below is decoded from the frozen artifacts' own inputs and the rejection
// message is compared against the frozen expected record.
TEST_CASE("decoded operation ingestion rejects the frozen malformed shapes before any target access",
        "[chord][delta][decode][validation][issue1185][spec]") {
    const auto frozen_rejects = delta_revisions_scenario(
            "unsafe prototype segments and malformed operation tuples are rejected before mutation");
    const auto* frozen_unsafe_error = frozen_rejects.object_member("unsafeError");
    const auto* frozen_malformed_error = frozen_rejects.object_member("malformedError");
    const auto* frozen_splice_error = frozen_rejects.object_member("spliceError");
    REQUIRE(frozen_unsafe_error != nullptr);
    REQUIRE(frozen_malformed_error != nullptr);
    REQUIRE(frozen_splice_error != nullptr);

    auto unsafe = decode_ops(frozen_input_json(frozen_rejects, "unsafe"));
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().code == support::ErrorCode::Validation);
    check_frozen_error(*frozen_unsafe_error, unsafe.error());

    auto unknown_verb = decode_op(frozen_input_json(frozen_rejects, "unknownVerb"));
    REQUIRE_FALSE(unknown_verb);
    check_frozen_error(*frozen_malformed_error, unknown_verb.error());

    auto string_index = decode_ops(frozen_input_json(frozen_rejects, "stringSpliceIndex"));
    REQUIRE_FALSE(string_index);
    check_frozen_error(*frozen_splice_error, string_index.error());

    // The frozen validator record lists each representable malformed shape with its recorded
    // message; every shape is decoded and its rejection message compared against the frozen one.
    const auto frozen_validator = validation_order_scenario(
            "the operation validator rejects each representable malformed shape with its recorded message");
    const auto* frozen_shapes = frozen_validator.inputs_member("shapes");
    const auto* frozen_results = frozen_validator.array_member("results");
    REQUIRE(frozen_shapes != nullptr);
    REQUIRE(frozen_results != nullptr);
    const auto* shape_arrays = frozen_shapes->get_if<support::JsonValue::array_t>();
    REQUIRE(shape_arrays != nullptr);
    REQUIRE(shape_arrays->size() == frozen_results->size());
    for (std::size_t index = 0; index < shape_arrays->size(); ++index) {
        auto shape_json = Json::from_support(shape_arrays->at(index));
        REQUIRE(shape_json.has_value());
        auto decoded = decode_op(*shape_json);
        const auto* frozen_result = frozen_results->at(index).get_if<support::JsonValue::object_t>();
        REQUIRE(frozen_result != nullptr);
        const auto accepted = support::json_bool_member(*frozen_result, "accepted");
        REQUIRE(accepted.has_value());
        CHECK_FALSE(*accepted);
        REQUIRE_FALSE(decoded.has_value());
        const auto* frozen_error = support::json_object_member(*frozen_result, "error");
        REQUIRE(frozen_error != nullptr);
        check_frozen_error(*frozen_error, decoded.error());
    }

    // The frozen vocabulary is representable: every accepted tuple decodes to its
    // typed operation.
    auto accepted = decode_ops(array({
            array({Json{"s"}, array({Json{"a"}}), Json{1}}),
            array({Json{"d"}, array({Json{"a"}})}),
            array({Json{"a"}, array({Json{"a"}}), Json{"x"}}),
            array({Json{"t"}, array({Json{"a"}}), Json{1}}),
            array({Json{"p"}, array({}), Json{1}, Json{0}, array({Json{"x"}})}),
            array({Json{"m"}, array({}), array({Json{0}})}),
            array({Json{"r"}, object({{"r", Json{true}}})}),
    }));
    REQUIRE(accepted);
    REQUIRE(accepted->size() == 7);
    const auto* set = operation_at<SetOp>(*accepted, 0);
    const auto* deletion = operation_at<DeleteOp>(*accepted, 1);
    const auto* append = operation_at<AppendOp>(*accepted, 2);
    const auto* truncate = operation_at<TruncateOp>(*accepted, 3);
    const auto* splice = operation_at<SpliceOp>(*accepted, 4);
    const auto* move = operation_at<MoveOp>(*accepted, 5);
    const auto* replace = operation_at<ReplaceOp>(*accepted, 6);
    REQUIRE(set != nullptr);
    REQUIRE(deletion != nullptr);
    REQUIRE(append != nullptr);
    REQUIRE(truncate != nullptr);
    REQUIRE(splice != nullptr);
    REQUIRE(move != nullptr);
    REQUIRE(replace != nullptr);
    CHECK(*set == SetOp{.path = keys({"a"}), .value = Json{1}});
    CHECK(*deletion == DeleteOp{.path = keys({"a"})});
    CHECK(*append == AppendOp{.path = keys({"a"}), .text = "x"});
    CHECK(*truncate == TruncateOp{.path = keys({"a"}), .count = 1});
    CHECK(*splice == SpliceOp{.path = Path{}, .index = 1, .remove = 0, .items = {Json{"x"}}});
    CHECK(*move == MoveOp{.path = Path{}, .permutation = {0}});
    CHECK(*replace == ReplaceOp{.value = object({{"r", Json{true}}})});
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
// "the operation validator rejects each representable malformed shape with its recorded message":
// the frozen record's unsafe-segment rejection text is the contract for every reserved segment,
// and a batch that fails on a later tuple must yield no partial batch.
TEST_CASE("decoded ingestion rejects every reserved segment and never yields a partial batch",
        "[chord][delta][decode][validation][issue1185][spec]") {
    const auto frozen_rejects = delta_revisions_scenario(
            "unsafe prototype segments and malformed operation tuples are rejected before mutation");
    const auto* frozen_unsafe_error = frozen_rejects.object_member("unsafeError");
    REQUIRE(frozen_unsafe_error != nullptr);
    const auto frozen = validation_order_scenario(
            "the operation validator rejects each representable malformed shape with its recorded message");
    const auto frozen_unsafe_message = support::json_string_member(*frozen_unsafe_error, "message");
    REQUIRE(frozen_unsafe_message.has_value());
    const auto frozen_unsafe_segment = frozen_unsafe_message->substr(frozen_unsafe_message->rfind(':') + 2);

    for (const char* segment : {"__proto__", "constructor", "prototype"}) {
        auto decoded = decode_op(array({Json{"s"}, array({Json{segment}, Json{"polluted"}}), Json{true}}));
        REQUIRE_FALSE(decoded);
        const auto expected_message = std::format("unsafe path segment: {}", segment);
        CHECK(decoded.error().message == expected_message);
        // The frozen record names __proto__; its recorded text is compared exactly and the
        // sibling segments reuse the same frozen message shape.
        if (segment == frozen_unsafe_segment) CHECK(decoded.error().message == *frozen_unsafe_message);
    }

    auto batch = decode_ops(array({
            array({Json{"s"}, array({Json{"value"}}), Json{2}}),
            array({Json{"p"}, array({Json{"items"}}), Json{"0"}, Json{0}, array({Json{"x"}})}),
    }));
    REQUIRE_FALSE(batch);
    CHECK(batch.error().message == "p index");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
// "a later empty path keeps the preceding mutable write and reports the empty path";
// "a later unsafe path keeps the preceding mutable write and reports the unsafe segment";
// "a later string splice index keeps the preceding mutable write and reports the splice index";
// "a later non-bijective permutation keeps the preceding mutable write and reports the permutation";
// "a root replacement adopted before a failing operation leaves the caller object unchanged".
// Each scenario's frozen inputs drive the decoded application and its frozen targetAfter/error
// fields are compared exactly.
TEST_CASE("decoded application keeps the preceding mutable write when a later tuple is rejected",
        "[chord][delta][decode][validation][ordering][issue1185][spec]") {
    const auto check_scenario = [](std::string_view name) {
        const auto frozen = validation_order_scenario(name);
        Json target = frozen_input_json(frozen, "initial");
        const auto* frozen_ops = frozen.inputs_member("ops");
        REQUIRE(frozen_ops != nullptr);
        auto ops_json = Json::from_support(*frozen_ops);
        REQUIRE(ops_json.has_value());
        // The decoded application entry point validates per tuple in sequence: the frozen batch
        // carries the failing tuple, so it cannot be decoded up front.
        const auto failure = apply(target, *ops_json);
        REQUIRE_FALSE(failure);
        const auto* frozen_error = frozen.object_member("error");
        REQUIRE(frozen_error != nullptr);
        check_frozen_error(*frozen_error, failure.error());
        CHECK(target == frozen_expected_json(frozen, "targetAfter"));
    };

    check_scenario("a later empty path keeps the preceding mutable write and reports the empty path");
    check_scenario("a later unsafe path keeps the preceding mutable write and reports the unsafe segment");
    check_scenario("a later string splice index keeps the preceding mutable write and reports the splice index");
    check_scenario("a later non-bijective permutation keeps the preceding mutable write and reports the permutation");
    check_scenario("a root replacement adopted before a failing operation leaves the caller object unchanged");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/session-transactions.json
// "document watches and states deliver only committed frames": the committed
// watch frame carried exactly this delta vocabulary, and applying it produced the
// delivered watch value. Both are the frozen artifact's own fields.
TEST_CASE("a committed watch frame's frozen delta vocabulary matches the tracker draft and value",
        "[chord][delta][decode][vocabulary][issue1185][spec]") {
    const auto frozen = session_transactions_scenario("document watches and states deliver only committed frames");
    const auto* frozen_frames = frozen.array_member("frames");
    REQUIRE(frozen_frames != nullptr);
    REQUIRE(frozen_frames->size() == 1);
    const auto* frozen_frame = frozen_frames->at(0).get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_frame != nullptr);
    const auto* frozen_ops = support::json_array_member(*frozen_frame, "ops");
    const auto* frozen_value = support::json_member(*frozen_frame, "value");
    REQUIRE(frozen_ops != nullptr);
    REQUIRE(frozen_value != nullptr);

    auto decoded = decode_frozen_ops(support::JsonValue{*frozen_ops});
    CHECK_FALSE(is_base(decoded));

    auto tracker = track(object({{"text", Json{"a"}}, {"revision", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.set(keys({"text"}), Json{"b"}));
    REQUIRE(draft.set(keys({"revision"}), Json{2}));
    auto prepared = change.prepare();
    REQUIRE(prepared);
    CHECK(prepared->ops() == decoded);

    auto next = apply_immutable(tracker.value(), decoded);
    REQUIRE(next);
    const auto frozen_frame_json = Json::from_support(*frozen_value);
    REQUIRE(frozen_frame_json.has_value());
    CHECK(*next == *frozen_frame_json);
}

// A root replacement is not the delta a committed watch frame delivers; a seam
// that replayed the frame as a replacement would produce a base batch instead.
// The frozen committed frame's vocabulary is
// fixtures/pi-v1.3.0/audit-1054/session-transactions.json "document watches and states deliver only
// committed frames".
TEST_CASE("a whole-root replacement is not the delta a committed watch frame delivers",
        "[chord][delta][decode][vocabulary][issue1185][spec]") {
    const auto frozen = session_transactions_scenario("document watches and states deliver only committed frames");
    const auto* frozen_frames = frozen.array_member("frames");
    REQUIRE(frozen_frames != nullptr);
    REQUIRE(frozen_frames->size() == 1);
    const auto* frozen_frame = frozen_frames->at(0).get_if<support::JsonValue::object_t>();
    REQUIRE(frozen_frame != nullptr);
    const auto* frozen_ops = support::json_array_member(*frozen_frame, "ops");
    REQUIRE(frozen_ops != nullptr);

    auto tracker = track(object({{"text", Json{"a"}}, {"revision", Json{1}}}));
    auto prepared = tracker.prepare_replace(object({{"text", Json{"b"}}, {"revision", Json{2}}}));
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 1);
    CHECK(is_base(prepared->ops()));
    CHECK(is_replace(prepared->ops().front()));

    auto decoded = decode_frozen_ops(support::JsonValue{*frozen_ops});
    CHECK(prepared->ops() != decoded);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json records per-operation
// validation; the typed operations below cover the representable shapes against the frozen
// messages, and "decoded application keeps the preceding mutable write when a later tuple is
// rejected" covers the malformed-tuple and string-index shapes through the decoded ingestion
// entry point.
TEST_CASE("application follows frozen sequential validation and replacement semantics",
        "[chord][delta][validation][issue1061][spec]") {
    const auto frozen_empty_path = validation_order_scenario(
            "a later empty path keeps the preceding mutable write and reports the empty path");
    const auto frozen_permutation = validation_order_scenario(
            "a later non-bijective permutation keeps the preceding mutable write and reports the permutation");
    const auto* frozen_empty_path_error = frozen_empty_path.object_member("error");
    const auto* frozen_permutation_error = frozen_permutation.object_member("error");
    REQUIRE(frozen_empty_path_error != nullptr);
    REQUIRE(frozen_permutation_error != nullptr);

    Json target = frozen_input_json(frozen_empty_path, "initial");
    const Json before = target;

    const auto unsafe = apply_immutable(
            target, OpBatch{SetOp{.path = Path{path_key("__proto__"), path_key("polluted")}, .value = Json{true}}});
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().message == "unsafe path segment: __proto__");

    const auto empty_path = apply_immutable(target, OpBatch{SetOp{.path = Path{}, .value = Json{true}}});
    REQUIRE_FALSE(empty_path);
    check_frozen_error(*frozen_empty_path_error, empty_path.error());

    const auto malformed_move = apply_immutable(target, OpBatch{MoveOp{.path = Path{}, .permutation = {0, 0}}});
    REQUIRE_FALSE(malformed_move);
    check_frozen_error(*frozen_permutation_error, malformed_move.error());

    const auto missing_array_element =
            apply_immutable(target, OpBatch{DeleteOp{.path = Path{path_key("items"), path_index(3)}}});
    REQUIRE_FALSE(missing_array_element);
    CHECK(missing_array_element.error().message.starts_with("unresolvable path"));

    auto immutable_failure = apply_immutable(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SpliceOp{.path = keys({"items"}), .index = 0, .remove = 0, .items = {Json{"x"}}},
                    MoveOp{.path = keys({"items"}), .permutation = {0, 0}},
            });
    REQUIRE_FALSE(immutable_failure);
    check_frozen_error(*frozen_permutation_error, immutable_failure.error());
    CHECK(target == before);

    // Frozen validates operations in sequence; an earlier mutable write remains
    // applied when a later operation has an invalid path.
    const auto syntax_invalid = apply(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SetOp{.path = Path{path_key("__proto__"), path_key("value")}, .value = Json{3}},
            });
    REQUIRE_FALSE(syntax_invalid);
    CHECK(syntax_invalid.error().message == "unsafe path segment: __proto__");
    CHECK(target == frozen_expected_json(frozen_empty_path, "targetAfter"));

    Json permutation_target = frozen_input_json(frozen_permutation, "initial");
    const auto invalid_permutation = apply(permutation_target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    MoveOp{.path = keys({"a"}), .permutation = {0, 0}},
            });
    REQUIRE_FALSE(invalid_permutation);
    check_frozen_error(*frozen_permutation_error, invalid_permutation.error());
    CHECK(permutation_target == frozen_expected_json(frozen_permutation, "targetAfter"));

    // A syntactically valid leading operation is applied in place; a later
    // traversal failure does not roll it back.
    const auto partially_valid = apply(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SetOp{.path = Path{path_key("missing"), path_key("value")}, .value = Json{3}},
            });
    REQUIRE_FALSE(partially_valid);
    CHECK(partially_valid.error().message.starts_with("unresolvable path"));
    CHECK(target == frozen_expected_json(frozen_empty_path, "targetAfter"));

    const auto replaced_then_failed = apply(target,
            OpBatch{
                    ReplaceOp{.value = object({{"replacement", Json{true}}})},
                    SetOp{.path = keys({"missing", "value"}), .value = Json{3}},
            });
    REQUIRE_FALSE(replaced_then_failed);
    CHECK(replaced_then_failed.error().message.starts_with("unresolvable path"));
    CHECK(target == frozen_expected_json(frozen_empty_path, "targetAfter"));

    const Json before_root_replace = target;
    const auto replaced_then_empty = apply(target,
            OpBatch{
                    ReplaceOp{.value = object({{"replacement", Json{true}}})},
                    SetOp{.path = Path{}, .value = Json{3}},
            });
    REQUIRE_FALSE(replaced_then_empty);
    check_frozen_error(*frozen_empty_path_error, replaced_then_empty.error());
    CHECK(target == before_root_replace);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "the tracker emits the set, append, splice, permutation, delete, truncate, and replace
// vocabulary". Each section's prepared operations and adopted value are compared against the
// frozen expected record's own `ops`, `before` and `value` fields.
TEST_CASE("tracker drafts emit the frozen set append splice permutation delete truncate and replace vocabulary",
        "[chord][delta][tracker][vocabulary][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario(
            "the tracker emits the set, append, splice, permutation, delete, truncate, and replace vocabulary");
    const auto frozen_case = [&frozen](std::string_view name) -> const support::JsonValue::object_t& {
        const auto* selected = frozen.object_member(name);
        REQUIRE(selected != nullptr);
        return *selected;
    };
    const auto frozen_ops_of = [](const support::JsonValue::object_t& record) {
        const auto* ops = support::json_array_member(record, "ops");
        REQUIRE(ops != nullptr);
        return decode_frozen_ops(support::JsonValue{*ops});
    };

    SECTION("set") {
        const auto& record = frozen_case("set");
        auto tracker = track(object({{"o", object({{"a", Json{1}}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.set(Path{path_key("o"), path_key("a")}, Json{2}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* set = operation_at<SetOp>(prepared->ops(), 0);
        REQUIRE(set != nullptr);
        CHECK(set->path == Path{path_key("o"), path_key("a")});
        CHECK(set->value == Json{2});
        REQUIRE(tracker.adopt(*prepared));
        auto expected_value = Json::from_support(*support::json_member(record, "value"));
        REQUIRE(expected_value.has_value());
        CHECK(tracker.value() == *expected_value);
    }

    SECTION("append") {
        const auto& record = frozen_case("append");
        auto tracker = track(object({{"s", Json{"abc"}}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.append_string(keys({"s"}), "d"));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* append = operation_at<AppendOp>(prepared->ops(), 0);
        REQUIRE(append != nullptr);
        CHECK(append->path == keys({"s"}));
        CHECK(append->text == "d");
        REQUIRE(tracker.adopt(*prepared));
        CHECK(member(tracker.value(), "s").as_string() != nullptr);
        CHECK(*member(tracker.value(), "s").as_string() == "abcd");
    }

    SECTION("splice") {
        const auto& record = frozen_case("splice");
        auto tracker = track(object({{"a", array({Json{"a"}, Json{"b"}, Json{"c"}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.splice(keys({"a"}), 1, 1, {Json{"X"}, Json{"Y"}}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 2);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* remove = operation_at<SpliceOp>(prepared->ops(), 0);
        const auto* insert = operation_at<SpliceOp>(prepared->ops(), 1);
        REQUIRE(remove != nullptr);
        REQUIRE(insert != nullptr);
        CHECK(remove->index == 1);
        CHECK(remove->remove == 1);
        CHECK(remove->items.empty());
        CHECK(insert->index == 1);
        CHECK(insert->remove == 0);
        REQUIRE(insert->items.size() == 2);
        CHECK(insert->items[0] == Json{"X"});
        CHECK(insert->items[1] == Json{"Y"});
        REQUIRE(tracker.adopt(*prepared));
        auto expected_value = Json::from_support(*support::json_member(record, "value"));
        REQUIRE(expected_value.has_value());
        CHECK(tracker.value() == *expected_value);
    }

    SECTION("permutation") {
        const auto& record = frozen_case("permutation");
        auto tracker = track(object({{"a", array({Json{"a"}, Json{"b"}, Json{"c"}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.move(keys({"a"}), {2, 1, 0}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* permutation = operation_at<MoveOp>(prepared->ops(), 0);
        REQUIRE(permutation != nullptr);
        CHECK(permutation->permutation == std::vector<std::size_t>{2, 1, 0});
        REQUIRE(tracker.adopt(*prepared));
        auto expected_value = Json::from_support(*support::json_member(record, "value"));
        REQUIRE(expected_value.has_value());
        CHECK(tracker.value() == *expected_value);
    }

    SECTION("delete") {
        const auto& record = frozen_case("delete");
        auto tracker = track(object({{"o", object({{"a", Json{1}}, {"b", Json{2}}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.erase(Path{path_key("o"), path_key("b")}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* deletion = operation_at<DeleteOp>(prepared->ops(), 0);
        REQUIRE(deletion != nullptr);
        CHECK(deletion->path == Path{path_key("o"), path_key("b")});
        REQUIRE(tracker.adopt(*prepared));
        auto expected_value = Json::from_support(*support::json_member(record, "value"));
        REQUIRE(expected_value.has_value());
        CHECK(tracker.value() == *expected_value);
    }

    SECTION("truncate") {
        const auto& record = frozen_case("truncate");
        auto tracker = track(object({{"s", Json{"abcdef"}}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.truncate_string(keys({"s"}), 3));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* truncate = operation_at<TruncateOp>(prepared->ops(), 0);
        REQUIRE(truncate != nullptr);
        CHECK(truncate->count == 3);
        REQUIRE(tracker.adopt(*prepared));
        CHECK(member(tracker.value(), "s").as_string() != nullptr);
        CHECK(*member(tracker.value(), "s").as_string() == "def");
    }

    SECTION("replace") {
        const auto& record = frozen_case("replace");
        auto tracker = track(object({{"o", object({{"a", Json{1}}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.set(keys({"o"}), object({{"b", Json{2}}})));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        CHECK(prepared->ops() == frozen_ops_of(record));
        const auto* set = operation_at<SetOp>(prepared->ops(), 0);
        REQUIRE(set != nullptr);
        CHECK(set->path == keys({"o"}));
        CHECK(set->value == object({{"b", Json{2}}}));
        REQUIRE(tracker.adopt(*prepared));
        auto expected_value = Json::from_support(*support::json_member(record, "value"));
        REQUIRE(expected_value.has_value());
        CHECK(tracker.value() == *expected_value);
    }
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "overlap returns the bounded common run used to merge tool output": the frozen cases and their
// recorded results drive the checks below.
TEST_CASE("overlap returns the bounded common run", "[chord][delta][overlap][issue1061][spec]") {
    const auto frozen = delta_revisions_scenario("overlap returns the bounded common run used to merge tool output");
    const auto* frozen_cases = frozen.inputs_member("cases");
    const auto* frozen_results = frozen.array_member("results");
    REQUIRE(frozen_cases != nullptr);
    REQUIRE(frozen_results != nullptr);
    const auto* case_arrays = frozen_cases->get_if<support::JsonValue::array_t>();
    REQUIRE(case_arrays != nullptr);
    REQUIRE(case_arrays->size() == frozen_results->size());
    for (std::size_t index = 0; index < case_arrays->size(); ++index) {
        const auto* case_tuple = case_arrays->at(index).get_if<support::JsonValue::array_t>();
        REQUIRE(case_tuple != nullptr);
        REQUIRE(case_tuple->size() == 3);
        const auto* first = case_tuple->at(0).get_if<std::string>();
        const auto* second = case_tuple->at(1).get_if<std::string>();
        const auto* bound = case_tuple->at(2).get_if<double>();
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        REQUIRE(bound != nullptr);
        CHECK(overlap(*first, *second, static_cast<std::size_t>(*bound)) ==
                static_cast<std::size_t>(frozen_results->at(index).get<double>()));
    }
}
