// T6 (#432): `util::JsonValue` I/O is implemented without Glaze in
// `src/util/Json.cpp` (build-performance-plan Stage 5) so non-serialization
// translation units no longer parse Glaze. These tests pin the behavior that
// must stay byte- and semantics-compatible with the Glaze-backed path it
// replaced: compact byte-identical serialization (Glaze number thresholds,
// string escaping), strict JSON grammar with Glaze's tolerance choices
// (trailing content ignored, last duplicate key wins, whitespace-only
// containers accepted), and the `\u` escape / surrogate rules.
#include "util/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cch;

TEST_CASE("JsonValue serializes numbers byte-identically to the Glaze path", "[util][json][t6]") {
    // Glaze (zmij) number rules: shortest round-trip digits; fixed notation
    // while the first digit's decimal exponent is in [-4, 15]; scientific
    // (uppercase `E`, no `+`, no leading zero) outside that range.
    const std::pair<double, std::string> cases[] = {
        {42.0, "42"},
        {0.1, "0.1"},
        {-0.0, "-0"},
        {0.0, "0"},
        {0.0001, "0.0001"},
        {0.00001, "1E-5"},
        {1e15, "1000000000000000"},
        {1e16, "1E16"},
        {1.5e-7, "1.5E-7"},
        {123456789.123, "123456789.123"},
        {999999.999, "999999.999"},
        {1.2345678901234568e29, "1.2345678901234568E29"},
        {1e308, "1E308"},
        {5e-324, "5E-324"},
        {0.30000000000000004, "0.30000000000000004"},
        {1.0000000000000002, "1.0000000000000002"},
    };
    for (const auto& [value, expected] : cases) {
        const auto serialized = util::write_json(util::JsonValue{value});
        REQUIRE(serialized);
        CHECK(*serialized == expected);
    }
}

TEST_CASE("JsonValue serializes only the Glaze escape set, raw otherwise", "[util][json][t6]") {
    const auto escaped = util::write_json(util::JsonValue{std::string{"a\"b\\c\n\r\t\b\f"}});
    REQUIRE(escaped);
    CHECK(*escaped == R"("a\"b\\c\n\r\t\b\f")");

    // Control bytes and UTF-8 pass through raw, matching Glaze's writer.
    const auto controls = util::write_json(util::JsonValue{std::string{"a\x01\x1f\x7f", 4}});
    REQUIRE(controls);
    CHECK(*controls == std::string{"\"a\x01\x1f\x7f\""});

    const auto utf8 = util::write_json(util::JsonValue{std::string{"caf\xc3\xa9 \xf0\x9f\x98\x80"}});
    REQUIRE(utf8);
    CHECK(*utf8 == std::string{"\"caf\xc3\xa9 \xf0\x9f\x98\x80\""});

    const auto slash = util::write_json(util::JsonValue{"/"});
    REQUIRE(slash);
    CHECK(*slash == "\"/\"");
}

TEST_CASE("JsonValue serializer emits sorted keys and compact structure", "[util][json][t6]") {
    const util::JsonValue value = util::JsonValue::object_t{
        {"b", util::JsonValue::array_t{1.0, 2.0}},
        {"a", true},
        {"c", util::JsonValue{nullptr}},
    };
    const auto serialized = util::write_json(value);
    REQUIRE(serialized);
    CHECK(*serialized == R"({"a":true,"b":[1,2],"c":null})");
}

TEST_CASE("JsonValue parser handles whitespace-only containers like Glaze", "[util][json][t6]") {
    const std::pair<std::string, std::string> cases[] = {
        {"[ ]", "[]"},
        {"[\n\t]", "[]"},
        {"{ }", "{}"},
        {"[\t1 ,\n2 ]", "[1,2]"},
        {"[ 1 , 2 ]", "[1,2]"},
    };
    for (const auto& [input, expected] : cases) {
        const auto parsed = util::read_json(input);
        REQUIRE(parsed);
        const auto serialized = util::write_json(*parsed);
        REQUIRE(serialized);
        CHECK(*serialized == expected);
    }
}

TEST_CASE("JsonValue parser accepts trailing content like Glaze", "[util][json][t6]") {
    for (const std::string input : {"{}x", "42 zz", "{} garbage", "  {\n \"a\" : 1\n}  "}) {
        const auto parsed = util::read_json(input);
        REQUIRE(parsed);
    }
    const auto parsed = util::read_json("{\"a\":1} {\"b\":2}");
    REQUIRE(parsed);
    const auto serialized = util::write_json(*parsed);
    REQUIRE(serialized);
    CHECK(*serialized == R"({"a":1})");
}

TEST_CASE("JsonValue parser applies strict JSON grammar and Glaze tolerances", "[util][json][t6]") {
    // Rejected forms.
    for (const std::string input : {"", "   ", ".5", "5.", "007", "[1,2,]", "{\"a\":1,}",
                                    "\"abc", "\"\\q\"", "\"\\u12\"", "{\"a\":}", "{\"a\" 1}"}) {
        CHECK_FALSE(util::read_json(input));
    }
    // Numbers out of double range fail; duplicate keys keep the last value.
    CHECK_FALSE(util::read_json("1e999"));
    CHECK_FALSE(util::read_json("1e-999"));
    const auto duplicates = util::read_json(R"({"a":1,"a":2})");
    REQUIRE(duplicates);
    const auto serialized = util::write_json(*duplicates);
    REQUIRE(serialized);
    CHECK(*serialized == R"({"a":2})");
}

TEST_CASE("JsonValue parser decodes Unicode escapes with surrogate pairs", "[util][json][t6]") {
    const auto escaped = util::read_json(R"("\u00e9\uD83D\uDE00\u001b")");
    REQUIRE(escaped);
    const auto serialized = util::write_json(*escaped);
    REQUIRE(serialized);
    CHECK(*serialized == std::string{"\"\xc3\xa9\xf0\x9f\x98\x80\x1b\""});
    // Lone surrogates are rejected, matching Glaze.
    CHECK_FALSE(util::read_json(R"("\uD800")"));
    CHECK_FALSE(util::read_json(R"("\uDC00")"));
    CHECK_FALSE(util::read_json(R"("\uD83D\u0041")"));
}

TEST_CASE("JsonValue parse errors are typed and carry context", "[util][json][t6]") {
    const auto malformed = util::read_json(R"({"a":1,})");
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == util::ErrorCode::JsonParse);
    CHECK(malformed.error().message == "failed to parse JSON");
    CHECK_FALSE(malformed.error().detail.empty());
    REQUIRE(malformed.error().context);
    CHECK(malformed.error().context->find("a") != std::string::npos);
}

TEST_CASE("JsonValue round-trips nested structures byte-identically", "[util][json][t6]") {
    // Object keys are map-sorted (matching the Glaze path), so the input uses
    // sorted keys for a byte-identical round-trip.
    const std::string input =
        R"({"arr":[1,2.5,"x",null],"flag":false,"n":0.1,"neg":-42.5,"obj":{"k":"v"},"text":"hello\nworld"})";
    const auto parsed = util::read_json(input);
    REQUIRE(parsed);
    const auto serialized = util::write_json(*parsed);
    REQUIRE(serialized);
    CHECK(*serialized == input);
}
