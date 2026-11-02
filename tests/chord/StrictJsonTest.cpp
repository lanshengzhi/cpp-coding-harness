#include <cch/chord/Json.hpp>
#include <cch/chord/StrictJson.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

using namespace cch;
using namespace cch::chord;
using namespace cch::support;

namespace {

constexpr const char* kNonFiniteMessage = "value contains a non-finite number and is not strict JSON";

[[nodiscard]] JsonValue object_with_nested_array() {
    return JsonValue{JsonValue::object_t{
            {"keep",
                    JsonValue{JsonValue::object_t{
                            {"nested", JsonValue{JsonValue::array_t{1.0, 2.0}}},
                    }}},
            {"value", 3.0},
    }};
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
// The C++ JsonValue cannot represent `undefined`; the detached-copy half is representable and
// `omit` is vacuous because an absent member is already omitted. The frozen input's `keep` and
// `value` members are compared; the copy is mutated in the frozen direction so a retained alias
// would change the source.
TEST_CASE("copy strict JSON returns a detached copy of the frozen value", "[chord][json][issue1060][issue1186][spec]") {
    auto source = object_with_nested_array();

    auto copied = copy_strict_json(source);
    REQUIRE(copied);

    auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);
    auto* copied_nested = nested_array(*copied_fields);
    REQUIRE(copied_nested != nullptr);
    REQUIRE(copied_nested->size() == 2);
    CHECK(copied_nested->at(0).get_number() == 1.0);
    CHECK(copied_nested->at(1).get_number() == 2.0);
    CHECK(copied_fields->at("value").get_number() == 3.0);
    CHECK(copied_fields->contains("omit") == false);

    // Frozen `copied.keep.nested.push(3)` leaves `sourceAfterNestedPush` at [1,2].
    copied_nested->push_back(JsonValue{3.0});
    CHECK(copied_nested->size() == 3);

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    auto* source_nested = nested_array(*source_fields);
    REQUIRE(source_nested != nullptr);
    CHECK(source_nested->size() == 2);
    CHECK(source_fields->at("value").get_number() == 3.0);

    source_nested->push_back(JsonValue{4.0});
    CHECK(copied_nested->size() == 3);
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
// "copyJson rejects functions, undefined array entries, non-finite numbers, class instances, and sparse arrays".
// The non-finite class is the one support::JsonValue can represent; the other classes are excluded
// by the carrier and are documented in the delivery report rather than normalized.
TEST_CASE("strict JSON rejects non-finite numbers at any depth", "[chord][json][issue1060][issue1186][spec]") {
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
        CHECK(copied.error().message == kNonFiniteMessage);
    }

    const JsonValue nested = JsonValue{JsonValue::object_t{
            {"items", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{std::nan("")}}}},
    }};
    CHECK_FALSE(is_strict_json_value(nested));
    const auto copied = copy_strict_json(nested);
    REQUIRE_FALSE(copied);
    CHECK(copied.error().code == ErrorCode::Validation);
    CHECK(copied.error().message == kNonFiniteMessage);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "copyJson copies strict JSON without retaining aliases".
// JavaScript reference identity has no C++ spelling in a value carrier; the observable property is
// mutation separation: the copy, its equal sibling member and the source stay independent.
TEST_CASE("copy strict JSON keeps equal sibling members independent of the source", "[chord][json][issue1186][spec]") {
    auto source = JsonValue{JsonValue::object_t{
            {"left", JsonValue{JsonValue::object_t{{"value", JsonValue{1.0}}}}},
            {"right", JsonValue{JsonValue::object_t{{"value", JsonValue{1.0}}}}},
    }};

    auto copied = copy_strict_json(source);
    REQUIRE(copied);
    auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);

    auto* left = copied_fields->at("left").get_if<JsonValue::object_t>();
    REQUIRE(left != nullptr);
    left->at("value") = JsonValue{2.0};

    auto* right = copied_fields->at("right").get_if<JsonValue::object_t>();
    REQUIRE(right != nullptr);
    CHECK(right->at("value").get_number() == 1.0);

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    auto* source_left = source_fields->at("left").get_if<JsonValue::object_t>();
    REQUIRE(source_left != nullptr);
    CHECK(source_left->at("value").get_number() == 1.0);
    auto* source_right = source_fields->at("right").get_if<JsonValue::object_t>();
    REQUIRE(source_right != nullptr);
    CHECK(source_right->at("value").get_number() == 1.0);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "copyJson preserves null prototypes and own __proto__ data properties".
// The null prototype is not representable by support::JsonValue; the own `__proto__` data member
// is, and copying it must not reinterpret the key as prototype syntax.
TEST_CASE("copy strict JSON preserves an own __proto__ data member", "[chord][json][issue1186][spec]") {
    auto source = JsonValue{JsonValue::object_t{
            {"__proto__", JsonValue{JsonValue::object_t{{"safe", JsonValue{true}}}}},
    }};

    const auto copied = copy_strict_json(source);
    REQUIRE(copied);
    auto* copied_fields = object_of(*copied);
    REQUIRE(copied_fields != nullptr);
    CHECK(copied_fields->contains("__proto__"));
    const auto* safe = copied_fields->at("__proto__").get_if<JsonValue::object_t>();
    REQUIRE(safe != nullptr);
    CHECK(safe->at("safe").get_boolean());

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    CHECK(source_fields->contains("__proto__"));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057-typed-json/chord-typed-json.json
// "isJsonValue checks strict JSON without normalizing it".
// The strict and non-finite cases are representable; `undefined`, Uint8Array and cycles are
// excluded by the carrier and have no C++ value to construct.
TEST_CASE("is strict JSON checks a direct JsonValue model without normalizing it", "[chord][json][issue1186][spec]") {
    const auto strict_value = JsonValue{JsonValue::object_t{
            {"nested", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{true}, JsonValue{nullptr}}}},
    }};
    CHECK(is_strict_json_value(strict_value));

    const auto non_finite = JsonValue{JsonValue::object_t{
            {"nested", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{std::nan("")}}}},
    }};
    CHECK_FALSE(is_strict_json_value(non_finite));

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
// "copyJson copies strict JSON without retaining aliases".
// The immutable Chord value is the durable ownership carrier; a copy shares containers, while an
// independently admitted value does not. The durable store itself is #1180's document boundary.
TEST_CASE("chord JSON admission detaches the support value and preserves immutable ownership",
        "[chord][json][issue1186][spec]") {
    auto source = object_with_nested_array();

    const auto admitted = Json::from_support(source);
    REQUIRE(admitted);

    auto* source_fields = object_of(source);
    REQUIRE(source_fields != nullptr);
    auto* source_nested = nested_array(*source_fields);
    REQUIRE(source_nested != nullptr);
    source_nested->push_back(JsonValue{4.0});

    const auto round_trip = admitted->to_support();
    const auto* round_trip_fields = round_trip.get_if<JsonValue::object_t>();
    REQUIRE(round_trip_fields != nullptr);
    const auto* round_trip_keep = round_trip_fields->at("keep").get_if<JsonValue::object_t>();
    REQUIRE(round_trip_keep != nullptr);
    const auto* round_trip_nested = round_trip_keep->at("nested").get_if<JsonValue::array_t>();
    REQUIRE(round_trip_nested != nullptr);
    CHECK(round_trip_nested->size() == 2);

    const Json same_container = *admitted;
    CHECK(same_container.shares_container_with(*admitted));

    const auto independently_admitted = Json::from_support(round_trip);
    REQUIRE(independently_admitted);
    CHECK_FALSE(independently_admitted->shares_container_with(*admitted));
}

TEST_CASE("chord JSON admission rejects a non-finite support value at any depth", "[chord][json][issue1186][spec]") {
    const auto accepted = Json::from_support(JsonValue{JsonValue::object_t{
            {"items", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{2.0}}}},
    }});
    REQUIRE(accepted);

    const auto rejected = Json::from_support(JsonValue{JsonValue::object_t{
            {"items", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{std::nan("")}}}},
    }});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::Validation);
    CHECK(rejected.error().message == kNonFiniteMessage);
}
