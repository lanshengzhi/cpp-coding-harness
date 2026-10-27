#include "support/Json.hpp"

#include <cch/support/JsonObject.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace cch::tests {
namespace {

/// A mapped type with equality, so the comparison members are instantiated.
/// `support::JsonValue` itself deliberately has no `operator==`; its objects are
/// compared through `support::canonical_json` text.
struct Value {
    int number{0};
    [[nodiscard]] bool operator==(const Value&) const = default;
};

template <typename T> [[nodiscard]] std::string keys_of(const support::JsonObject<T>& object) {
    std::string keys;
    for (const auto& [key, value] : object) {
        (void)value;
        keys += key;
    }
    return keys;
}

} // namespace

TEST_CASE("JsonObject iterates members in insertion order", "[support][json][issue1149][spec]") {
    support::JsonObject<Value> object;
    object.emplace("b", Value{2});
    object.emplace("a", Value{1});
    object.emplace("c", Value{3});
    CHECK(keys_of(object) == "bac");

    // A re-inserted key keeps its original position, as `std::map::emplace` did.
    object.emplace("a", Value{9});
    CHECK(keys_of(object) == "bac");
    CHECK(object.at("a").number == 1);
    CHECK(object.size() == 3);
}

TEST_CASE("JsonObject insert_or_assign keeps the position and replaces the value", "[support][json][issue1149][spec]") {
    support::JsonObject<Value> object;
    object.emplace("b", Value{2});
    object.emplace("a", Value{1});
    const auto [position, inserted] = object.insert_or_assign("b", Value{9});
    CHECK_FALSE(inserted);
    CHECK(position->first == "b");
    CHECK(keys_of(object) == "ba");
    CHECK(object.at("b").number == 9);

    const auto [appended, appended_flag] = object.insert_or_assign("z", Value{0});
    CHECK(appended_flag);
    CHECK(keys_of(object) == "baz");
}

TEST_CASE("JsonObject equality ignores member order and same_order does not", "[support][json][issue1149][spec]") {
    support::JsonObject<Value> forward;
    forward.emplace("a", Value{1});
    forward.emplace("b", Value{2});
    support::JsonObject<Value> reverse;
    reverse.emplace("b", Value{2});
    reverse.emplace("a", Value{1});

    // Object member order is not part of a JSON value.
    CHECK(forward == reverse);
    CHECK_FALSE(forward.same_order(reverse));
    CHECK(forward.same_order(forward));

    reverse.insert_or_assign("b", Value{3});
    CHECK_FALSE(forward == reverse);
}

TEST_CASE("JsonObject erase reports whether the key was present", "[support][json][issue1149][spec]") {
    support::JsonObject<Value> object;
    object.emplace("b", Value{2});
    object.emplace("a", Value{1});
    object.emplace("c", Value{3});

    CHECK(object.erase("a") == 1);
    CHECK(object.erase("a") == 0);
    CHECK(keys_of(object) == "bc");

    const auto position = object.find("c");
    REQUIRE(position != object.end());
    const auto next = object.erase(position);
    CHECK(next == object.end());
    CHECK(keys_of(object) == "b");
}

TEST_CASE("JsonObject initializer list keeps the first value of a repeated key", "[support][json][issue1149][spec]") {
    // `std::map`'s initializer-list constructor kept the first duplicate; the
    // ordered form keeps the same rule so a literal behaves identically.
    const support::JsonObject<Value> object{
            {"a", Value{1}},
            {"b", Value{2}},
            {"a", Value{9}},
    };
    CHECK(keys_of(object) == "ab");
    CHECK(object.at("a").number == 1);
}

TEST_CASE("a parsed object keeps the source text's member order", "[support][json][issue1149][spec]") {
    const auto parsed = support::read_json(R"({"b":1,"a":2,"c":3})");
    REQUIRE(parsed);
    const auto* object = parsed->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    CHECK(keys_of(*object) == "bac");
}

} // namespace cch::tests
