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

[[nodiscard]] JsonValue object_with_nested_array() {
    return JsonValue{JsonValue::object_t{
            {"keep",
                    JsonValue{JsonValue::object_t{
                            {"nested", JsonValue{JsonValue::array_t{1.0, 2.0}}},
                    }}},
            {"value", 3.0},
    }};
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "copyJson returns a detached strict-JSON value and omits undefined object properties".
// The C++ JsonValue cannot represent `undefined`; the detached-copy half is representable.
TEST_CASE("copy strict JSON returns a detached copy of the frozen value", "[chord][json][issue1060][spec]") {
    auto source = object_with_nested_array();

    const auto copied = copy_strict_json(source);
    REQUIRE(copied);

    const auto* copied_object = copied->get_if<JsonValue::object_t>();
    REQUIRE(copied_object != nullptr);
    const auto* keep = copied_object->at("keep").get_if<JsonValue::object_t>();
    REQUIRE(keep != nullptr);
    const auto* nested = keep->at("nested").get_if<JsonValue::array_t>();
    REQUIRE(nested != nullptr);
    REQUIRE(nested->size() == 2);
    CHECK(nested->at(0).get_number() == 1.0);
    CHECK(nested->at(1).get_number() == 2.0);
    CHECK(copied_object->at("value").get_number() == 3.0);

    auto* source_object = source.get_if<JsonValue::object_t>();
    REQUIRE(source_object != nullptr);
    auto* source_keep = source_object->at("keep").get_if<JsonValue::object_t>();
    REQUIRE(source_keep != nullptr);
    auto* source_nested = source_keep->at("nested").get_if<JsonValue::array_t>();
    REQUIRE(source_nested != nullptr);
    source_nested->push_back(JsonValue{4.0});

    CHECK(nested->size() == 2);
    CHECK(copied_object->at("value").get_number() == 3.0);
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
// The non-finite case is the one support::JsonValue can represent.
TEST_CASE("strict JSON rejects non-finite numbers at any depth", "[chord][json][issue1060][spec]") {
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
        CHECK(copied.error().message == "value contains a non-finite number and is not strict JSON");
    }

    const JsonValue nested = JsonValue{JsonValue::object_t{
            {"items", JsonValue{JsonValue::array_t{JsonValue{1.0}, JsonValue{std::nan("")}}}},
    }};
    CHECK_FALSE(is_strict_json_value(nested));
    const auto copied = copy_strict_json(nested);
    REQUIRE_FALSE(copied);
    CHECK(copied.error().code == ErrorCode::Validation);
}
