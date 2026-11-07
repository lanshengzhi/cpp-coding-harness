#include <cch/chord/Json.hpp>
#include <cch/chord/StrictJson.hpp>

#include <cch/support/JsonAccess.hpp>

#include "support/ChordEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

using namespace cch;
using namespace cch::chord;
using namespace cch::support;

namespace {

constexpr const char* kNonFiniteMessage = "value contains a non-finite number and is not strict JSON";

/// The frozen #1057 json-output-support observation.
[[nodiscard]] cch::tests::FrozenChordScenario json_output_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1057", "json-output-support.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen #1057 typed-json observation.
[[nodiscard]] cch::tests::FrozenChordScenario typed_json_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1057-typed-json", "chord-typed-json.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

/// The frozen diagnostics begin with a capital `Value`; the delivered C++ diagnostics begin
/// with lowercase `value` (src/chord/StrictJson.cpp). Only that initial letter is normalized;
/// the rest of each sentence is compared exactly (recorded in the #1186 delivery report).
[[nodiscard]] std::string frozen_normalized_message(std::string_view frozen_message) {
    std::string normalized{frozen_message};
    REQUIRE(!normalized.empty());
    normalized[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(normalized[0])));
    return normalized;
}

[[nodiscard]] JsonValue frozen_nested_non_finite(const cch::tests::FrozenChordScenario& frozen) {
    const auto* input = frozen.inputs_member("value");
    REQUIRE(input != nullptr);
    auto nested = *input;
    auto* outer = nested.get_if<JsonValue::object_t>();
    REQUIRE(outer != nullptr);
    auto* inner = outer->at("outer").get_if<JsonValue::object_t>();
    REQUIRE(inner != nullptr);
    auto* values = inner->at("inner").get_if<JsonValue::array_t>();
    REQUIRE(values != nullptr);
    REQUIRE(values->size() == 2);
    REQUIRE(values->at(1).holds<std::string>());
    REQUIRE(values->at(1).get_string() == "NaN");
    values->at(1) = JsonValue{std::nan("")};
    return nested;
}

[[nodiscard]] bool matches_frozen_failure(const support::Expected<JsonValue>& result, std::string_view message) {
    return !result && result.error().code == ErrorCode::Validation &&
           result.error().message == frozen_normalized_message(message);
}

[[nodiscard]] JsonValue::object_t* object_of(JsonValue& value) { return value.get_if<JsonValue::object_t>(); }

[[nodiscard]] const JsonValue::object_t* object_of(const JsonValue& value) {
    return value.get_if<JsonValue::object_t>();
}

[[nodiscard]] JsonValue::array_t* nested_array(JsonValue::object_t& object) {
    auto* keep = object.at("keep").get_if<JsonValue::object_t>();
    if (keep == nullptr) return nullptr;
    return keep->at("nested").get_if<JsonValue::array_t>();
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "copyJson returns a detached strict-JSON value and omits undefined object properties".
// The C++ JsonValue cannot represent `undefined`; the frozen input's `keep` and `omit` members
// are read from the artifact, the `omit` placeholder is dropped as unrepresentable, and the copy
// is mutated in the frozen direction so a retained alias would change the source. The frozen
// `copied`, `copiedIsDetached`, `copiedHasOmit` and `sourceAfterNestedPush` fields are compared.
TEST_CASE("copy strict JSON returns a detached copy of the frozen value", "[chord][json][issue1060][issue1186][spec]") {
    const auto frozen =
            json_output_scenario("copyJson returns a detached strict-JSON value and omits undefined object properties");
    const auto* frozen_value = frozen.inputs_member("value");
    REQUIRE(frozen_value != nullptr);
    const auto* frozen_fields = frozen_value->get_if<JsonValue::object_t>();
    REQUIRE(frozen_fields != nullptr);
    const auto* frozen_keep = support::json_object_member(*frozen_fields, "keep");
    REQUIRE(frozen_keep != nullptr);
    // The frozen input's `omit` member names the undefined placeholder the C++ carrier cannot
    // represent; the copy below is built from the artifact's own `keep` member.
    auto source = JsonValue{JsonValue::object_t{{"keep", JsonValue{*frozen_keep}}}};

    auto copied = copy_strict_json(source);
    REQUIRE(copied);

    // Frozen `copiedIsDetached`: the frozen copy is a detached value.
    const auto copied_is_detached = frozen.boolean_member("copiedIsDetached");
    REQUIRE(copied_is_detached.has_value());
    CHECK(*copied_is_detached);

    // Frozen `copied.keep.nested.push(3)` leaves `sourceAfterNestedPush` at [1,2].
    auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);
    auto* copied_nested = nested_array(*copied_fields);
    REQUIRE(copied_nested != nullptr);
    REQUIRE(copied_nested->size() == 2);
    CHECK(copied_nested->at(0).get_number() == 1.0);
    CHECK(copied_nested->at(1).get_number() == 2.0);
    CHECK(copied_fields->contains("omit") == false);
    const auto copied_has_omit = frozen.boolean_member("copiedHasOmit");
    REQUIRE(copied_has_omit.has_value());
    CHECK(copied_fields->contains("omit") == *copied_has_omit);
    copied_nested->push_back(JsonValue{3.0});
    const auto* frozen_copied = frozen.object_member("copied");
    REQUIRE(frozen_copied != nullptr);
    const auto* frozen_copied_value = support::json_member(*frozen_copied, "keep");
    REQUIRE(frozen_copied_value != nullptr);
    const auto* frozen_copied_nested =
            support::json_array_member(*frozen_copied_value->get_if<JsonValue::object_t>(), "nested");
    REQUIRE(frozen_copied_nested != nullptr);
    REQUIRE(copied_nested->size() == frozen_copied_nested->size());
    for (std::size_t index = 0; index < frozen_copied_nested->size(); ++index) {
        CHECK(copied_nested->at(index).get_number() == frozen_copied_nested->at(index).get_number());
    }

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    auto* source_nested = nested_array(*source_fields);
    REQUIRE(source_nested != nullptr);
    const auto* frozen_source_after = frozen.object_member("sourceAfterNestedPush");
    REQUIRE(frozen_source_after != nullptr);
    const auto* frozen_source_nested =
            support::json_array_member(*support::json_object_member(*frozen_source_after, "keep"), "nested");
    REQUIRE(frozen_source_nested != nullptr);
    CHECK(source_nested->size() == frozen_source_nested->size());
    for (std::size_t index = 0; index < frozen_source_nested->size(); ++index) {
        CHECK(source_nested->at(index).get_number() == frozen_source_nested->at(index).get_number());
    }

    source_nested->push_back(JsonValue{4.0});
    CHECK(copied_nested->size() == frozen_copied_nested->size());
}

TEST_CASE("strict JSON accepts every representable JSON value", "[chord][json][issue1060][spec]") {
    CHECK(is_strict_json_value(JsonValue{nullptr}));
    CHECK(is_strict_json_value(JsonValue{true}));
    CHECK(is_strict_json_value(JsonValue{std::string{"text"}}));
    CHECK(is_strict_json_value(JsonValue{0.0}));
    CHECK(is_strict_json_value(JsonValue{-1.5}));
    CHECK(is_strict_json_value(JsonValue{JsonValue::array_t{JsonValue{nullptr}, JsonValue{1.0}}}));
    CHECK(is_strict_json_value(JsonValue{JsonValue::object_t{{"key", JsonValue{false}}}}));

    const auto copied = copy_strict_json(JsonValue{JsonValue::array_t{
            JsonValue{nullptr},
            JsonValue{true},
            JsonValue{std::string{"text"}},
            JsonValue{1.25},
    }});
    REQUIRE(copied);
    const auto* array = copied->get_if<JsonValue::array_t>();
    REQUIRE(array != nullptr);
    REQUIRE(array->size() == 4);
    CHECK(array->at(0).holds<std::nullptr_t>());
    CHECK(array->at(1).get_boolean());
    CHECK(array->at(2).get_string() == "text");
    CHECK(array->at(3).get_number() == 1.25);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "copyJson rejects functions, undefined array entries, non-finite numbers, class instances, and
// sparse arrays" and fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json "copyJson
// rejects a non-finite number nested below the root". The non-finite class is the one
// support::JsonValue can represent; the delivered message is compared against the frozen text
// with the recorded initial-letter normalization, and the frozen error names are checked as
// recorded.
TEST_CASE("strict JSON rejects non-finite numbers at any depth", "[chord][json][issue1060][issue1186][spec]") {
    const auto frozen_rejects = json_output_scenario(
            "copyJson rejects functions, undefined array entries, non-finite numbers, class instances, and sparse "
            "arrays");
    const auto frozen_nested = typed_json_scenario("copyJson rejects a non-finite number nested below the root");
    const auto* frozen_failures = frozen_rejects.object_member("failures");
    const auto* frozen_nested_failure = frozen_nested.object_member("failure");
    REQUIRE(frozen_failures != nullptr);
    REQUIRE(frozen_nested_failure != nullptr);
    const auto* frozen_non_finite = support::json_object_member(*frozen_failures, "nonFinite");
    REQUIRE(frozen_non_finite != nullptr);
    const auto frozen_non_finite_name = support::json_string_member(*frozen_non_finite, "name");
    const auto frozen_non_finite_message = support::json_string_member(*frozen_non_finite, "message");
    const auto frozen_nested_name = support::json_string_member(*frozen_nested_failure, "name");
    const auto frozen_nested_message = support::json_string_member(*frozen_nested_failure, "message");
    REQUIRE(frozen_non_finite_name.has_value());
    REQUIRE(frozen_non_finite_message.has_value());
    REQUIRE(frozen_nested_name.has_value());
    REQUIRE(frozen_nested_message.has_value());
    CHECK(*frozen_non_finite_name == "TypeError");
    CHECK(*frozen_nested_name == "TypeError");
    const auto expected_message = frozen_normalized_message(*frozen_non_finite_message);

    const double non_finite_values[] = {
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
    };

    for (const double value : non_finite_values) {
        CHECK_FALSE(is_strict_json_value(JsonValue{value}));

        const auto copied = copy_strict_json(JsonValue{value});
        REQUIRE_FALSE(copied);
        CHECK(copied.error().code == ErrorCode::Validation);
        CHECK(copied.error().message == expected_message);
        CHECK(copied.error().message == kNonFiniteMessage);
    }

    const JsonValue nested = frozen_nested_non_finite(frozen_nested);
    CHECK_FALSE(is_strict_json_value(nested));
    const auto copied = copy_strict_json(nested);
    CHECK(matches_frozen_failure(copied, *frozen_nested_message));
}

TEST_CASE("frozen nested JSON comparison rejects a root-only validator", "[chord][json][issue1240][spec]") {
    const auto frozen = typed_json_scenario("copyJson rejects a non-finite number nested below the root");
    const auto* failure = frozen.object_member("failure");
    REQUIRE(failure != nullptr);
    const auto message = support::json_string_member(*failure, "message");
    REQUIRE(message.has_value());
    const JsonValue nested = frozen_nested_non_finite(frozen);

    // This mutant validates a root number but never inspects an object's descendants.
    const auto root_only_copy = [](const JsonValue& value) -> support::Expected<JsonValue> {
        const auto* number = value.get_if<double>();
        if (number != nullptr && !std::isfinite(*number)) {
            return std::unexpected(support::make_error(ErrorCode::Validation, kNonFiniteMessage));
        }
        return value;
    };
    CHECK(matches_frozen_failure(root_only_copy(JsonValue{std::nan("")}), *message));
    CHECK_FALSE(matches_frozen_failure(root_only_copy(nested), *message));
    CHECK(matches_frozen_failure(copy_strict_json(nested), *message));
}

// The frozen placeholder names NaN, which the support value represents as a double.
// Rejecting this nested value distinguishes recursive validation from a root-only check.
TEST_CASE("copy strict JSON rejects the frozen nested non-finite value", "[chord][json][issue1186][issue1240][spec]") {
    const auto frozen = typed_json_scenario("copyJson rejects a non-finite number nested below the root");
    const auto* input = frozen.inputs_member("value");
    const auto* failure = frozen.object_member("failure");
    REQUIRE(input != nullptr);
    REQUIRE(failure != nullptr);
    const auto message = support::json_string_member(*failure, "message");
    REQUIRE(message.has_value());

    auto nested = *input;
    auto* outer = object_of(nested);
    REQUIRE(outer != nullptr);
    auto* inner = outer->at("outer").get_if<JsonValue::object_t>();
    REQUIRE(inner != nullptr);
    auto* values = inner->at("inner").get_if<JsonValue::array_t>();
    REQUIRE(values != nullptr);
    REQUIRE(values->size() == 2);
    REQUIRE(values->at(1).holds<std::string>());
    REQUIRE(values->at(1).get_string() == "NaN");
    values->at(1) = JsonValue{std::nan("")};

    const auto copied = copy_strict_json(nested);
    REQUIRE_FALSE(copied);
    CHECK(copied.error().code == ErrorCode::Validation);
    CHECK(copied.error().message == frozen_normalized_message(*message));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "copyJson copies strict JSON without retaining aliases". The frozen input value drives the
// copy; mutating the copy's `left` must leave `right` and the source at the frozen values, and
// the copy must match the frozen `copiedAfterMutation` record. The reference-identity fields
// (`copiedIsInput`, `leftIsShared`, `rightIsShared`, `leftIsRight`) have no C++ spelling in a
// value carrier; the compared property is mutation separation, the same ownership fact.
TEST_CASE("copy strict JSON keeps equal sibling members independent of the source", "[chord][json][issue1186][spec]") {
    const auto frozen = typed_json_scenario("copyJson copies strict JSON without retaining aliases");
    const auto* frozen_value = frozen.inputs_member("value");
    REQUIRE(frozen_value != nullptr);
    auto source = *frozen_value;

    auto copied = copy_strict_json(source);
    REQUIRE(copied);
    auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);

    auto* left = copied_fields->at("left").get_if<JsonValue::object_t>();
    REQUIRE(left != nullptr);
    left->at("value") = JsonValue{2.0};

    const auto* frozen_copied_after = frozen.object_member("copiedAfterMutation");
    const auto* frozen_source_after = frozen.object_member("sourceAfterCopyMutation");
    const auto frozen_shared_after = frozen.number_member("sharedValueAfterCopyMutation");
    REQUIRE(frozen_copied_after != nullptr);
    REQUIRE(frozen_source_after != nullptr);
    REQUIRE(frozen_shared_after.has_value());

    auto* right = copied_fields->at("right").get_if<JsonValue::object_t>();
    REQUIRE(right != nullptr);
    CHECK(right->at("value").get_number() == *frozen_shared_after);

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    const auto* frozen_source_left = support::json_object_member(*frozen_source_after, "left");
    const auto* frozen_source_right = support::json_object_member(*frozen_source_after, "right");
    REQUIRE(frozen_source_left != nullptr);
    REQUIRE(frozen_source_right != nullptr);
    auto* source_left = source_fields->at("left").get_if<JsonValue::object_t>();
    auto* source_right = source_fields->at("right").get_if<JsonValue::object_t>();
    REQUIRE(source_left != nullptr);
    REQUIRE(source_right != nullptr);
    const auto frozen_source_left_value = support::json_number_member(*frozen_source_left, "value");
    const auto frozen_source_right_value = support::json_number_member(*frozen_source_right, "value");
    REQUIRE(frozen_source_left_value.has_value());
    REQUIRE(frozen_source_right_value.has_value());
    CHECK(source_left->at("value").get_number() == *frozen_source_left_value);
    CHECK(source_right->at("value").get_number() == *frozen_source_right_value);

    // The mutated copy matches the frozen `copiedAfterMutation` record exactly.
    auto actual_text = support::canonical_json(*copied);
    auto expected_text = support::canonical_json(JsonValue{*frozen_copied_after});
    REQUIRE(actual_text.has_value());
    REQUIRE(expected_text.has_value());
    CHECK(*actual_text == *expected_text);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "copyJson preserves null prototypes and own __proto__ data properties". The frozen input value
// drives the copy; the frozen `copiedHasOwnProto`, `copiedProtoValue` and `copiedEqualsInputShape`
// fields are compared. The null prototype is not representable by support::JsonValue and is
// excluded by the carrier (recorded in the #1186 delivery report).
TEST_CASE("copy strict JSON preserves an own __proto__ data member", "[chord][json][issue1186][spec]") {
    const auto frozen = typed_json_scenario("copyJson preserves null prototypes and own __proto__ data properties");
    const auto* frozen_value = frozen.inputs_member("value");
    REQUIRE(frozen_value != nullptr);
    auto source = *frozen_value;

    const auto copied = copy_strict_json(source);
    REQUIRE(copied);
    const auto copied_has_own_proto = frozen.boolean_member("copiedHasOwnProto");
    const auto copied_equals_input_shape = frozen.boolean_member("copiedEqualsInputShape");
    REQUIRE(copied_has_own_proto.has_value());
    REQUIRE(copied_equals_input_shape.has_value());
    auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);
    CHECK(copied_fields->contains("__proto__") == *copied_has_own_proto);
    const auto* frozen_proto_value = frozen.member("copiedProtoValue");
    REQUIRE(frozen_proto_value != nullptr);
    const auto* copied_proto = copied_fields->at("__proto__").get_if<JsonValue::object_t>();
    REQUIRE(copied_proto != nullptr);
    const auto* frozen_proto_object = frozen_proto_value->get_if<JsonValue::object_t>();
    REQUIRE(frozen_proto_object != nullptr);
    const auto frozen_safe = support::json_bool_member(*frozen_proto_object, "safe");
    REQUIRE(frozen_safe.has_value());
    CHECK(copied_proto->at("safe").get_boolean() == *frozen_safe);

    // The copied value matches the frozen input shape exactly.
    auto actual_text = support::canonical_json(*copied);
    auto expected_text = support::canonical_json(source);
    REQUIRE(actual_text.has_value());
    REQUIRE(expected_text.has_value());
    CHECK((*actual_text == *expected_text) == *copied_equals_input_shape);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "isJsonValue checks strict JSON without normalizing it". The strict and non-finite cases are
// representable and are compared against the frozen `results` record; `undefined`, Uint8Array and
// cycles are excluded by the carrier and have no C++ value to construct.
TEST_CASE("is strict JSON checks a direct JsonValue model without normalizing it", "[chord][json][issue1186][spec]") {
    const auto frozen = typed_json_scenario("isJsonValue checks strict JSON without normalizing it");
    const auto* frozen_results = frozen.object_member("results");
    REQUIRE(frozen_results != nullptr);
    const auto frozen_strict = support::json_bool_member(*frozen_results, "strict");
    const auto frozen_infinity = support::json_bool_member(*frozen_results, "infinity");
    REQUIRE(frozen_strict.has_value());
    REQUIRE(frozen_infinity.has_value());

    const auto strict_value = JsonValue{JsonValue::object_t{
            {"nested", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{true}, JsonValue{nullptr}}}},
    }};
    CHECK(is_strict_json_value(strict_value) == *frozen_strict);

    const auto non_finite = JsonValue{JsonValue::object_t{
            {"nested", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{std::nan("")}}}},
    }};
    CHECK(is_strict_json_value(non_finite) == *frozen_infinity);

    const auto copied = copy_strict_json(strict_value);
    REQUIRE(copied);
    const auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);
    const auto* nested = copied_fields->at("nested").get_if<JsonValue::array_t>();
    REQUIRE(nested != nullptr);
    REQUIRE(nested->size() == 3);
    CHECK(nested->at(0).get_number() == 1.0);
    CHECK(nested->at(1).get_boolean());
    CHECK(nested->at(2).holds<std::nullptr_t>());
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "copyJson copies strict JSON without retaining aliases". The immutable Chord value is the
// durable ownership carrier; a copy shares containers, while an independently admitted value does
// not. The frozen input value drives the admission and the frozen `copiedBeforeMutation`,
// `copiedIsInput` and `copiedAfterMutation` records are compared; the reference-identity flags
// (`leftIsShared`, `rightIsShared`, `leftIsRight`) have no C++ spelling in a value carrier and
// are covered by mutation separation.
TEST_CASE("chord JSON admission detaches the support value and preserves immutable ownership",
        "[chord][json][issue1186][spec]") {
    const auto frozen = typed_json_scenario("copyJson copies strict JSON without retaining aliases");
    const auto* frozen_value = frozen.inputs_member("value");
    REQUIRE(frozen_value != nullptr);
    auto source = *frozen_value;

    const auto admitted = Json::from_support(source);
    REQUIRE(admitted);

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    auto* source_left = source_fields->at("left").get_if<JsonValue::object_t>();
    REQUIRE(source_left != nullptr);
    source_left->at("value") = JsonValue{2.0};

    const auto round_trip = admitted->to_support();
    const auto* frozen_before = frozen.object_member("copiedBeforeMutation");
    const auto* frozen_after = frozen.object_member("copiedAfterMutation");
    const auto copied_is_input = frozen.boolean_member("copiedIsInput");
    REQUIRE(frozen_before != nullptr);
    REQUIRE(frozen_after != nullptr);
    REQUIRE(copied_is_input.has_value());
    // The admitted value is detached from the mutated source: its round trip matches the frozen
    // pre-mutation record, not the source's post-mutation state.
    auto round_trip_text = support::canonical_json(round_trip);
    auto frozen_before_text = support::canonical_json(JsonValue{*frozen_before});
    REQUIRE(round_trip_text.has_value());
    REQUIRE(frozen_before_text.has_value());
    CHECK(*round_trip_text == *frozen_before_text);
    CHECK(!*copied_is_input);

    const Json same_container = *admitted;
    CHECK(same_container.shares_container_with(*admitted));

    const auto independently_admitted = Json::from_support(round_trip);
    REQUIRE(independently_admitted);
    CHECK_FALSE(independently_admitted->shares_container_with(*admitted));

    // Mutating the admitted value's own copy (the frozen mutation direction) isolates the frozen
    // sibling record.
    auto admitted_copy = *admitted;
    auto copy_value = admitted_copy.to_support();
    auto* copy_fields = object_of(copy_value);
    REQUIRE(copy_fields != nullptr);
    auto* copy_left = copy_fields->at("left").get_if<JsonValue::object_t>();
    REQUIRE(copy_left != nullptr);
    copy_left->at("value") = JsonValue{2.0};
    auto copy_text = support::canonical_json(copy_value);
    auto frozen_after_text = support::canonical_json(JsonValue{*frozen_after});
    REQUIRE(copy_text.has_value());
    REQUIRE(frozen_after_text.has_value());
    CHECK(*copy_text == *frozen_after_text);
}

TEST_CASE("chord JSON admission rejects a non-finite support value at any depth", "[chord][json][issue1186][spec]") {
    const auto frozen = typed_json_scenario("copyJson rejects a non-finite number nested below the root");
    const auto* frozen_failure = frozen.object_member("failure");
    REQUIRE(frozen_failure != nullptr);
    const auto frozen_name = support::json_string_member(*frozen_failure, "name");
    const auto frozen_message = support::json_string_member(*frozen_failure, "message");
    REQUIRE(frozen_name.has_value());
    REQUIRE(frozen_message.has_value());
    CHECK(*frozen_name == "TypeError");

    const auto accepted = Json::from_support(JsonValue{JsonValue::object_t{
            {"items", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{2.0}}}},
    }});
    CHECK(accepted);

    const auto rejected = Json::from_support(JsonValue{JsonValue::object_t{
            {"outer",
                    JsonValue{JsonValue::object_t{
                            {"inner", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{std::nan("")}}}}}}},
    }});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::Validation);
    CHECK(rejected.error().message == frozen_normalized_message(*frozen_message));
    CHECK(rejected.error().message == kNonFiniteMessage);
}
