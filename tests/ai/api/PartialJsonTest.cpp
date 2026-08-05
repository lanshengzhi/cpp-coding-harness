#include "ai/api/PartialJson.hpp"
#include "util/Json.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

struct StreamingJsonCase {
    std::string input;
    std::string expected;
};

[[nodiscard]] std::string parse(std::string_view input) {
    const auto value = ai::api::parse_streaming_json(input);
    const auto serialized = util::write_json(value);
    REQUIRE(serialized);
    return *serialized;
}

} // namespace

TEST_CASE(
    "streaming tool arguments parse with pi partial-json semantics",
    "[ai][api][issue370]") {
    // Expected values are the exact outputs of pi's `parseStreamingJson`
    // (the `partial-json` package, Allow.ALL) captured from the frozen pi
    // checkout; see fixtures/pi-ai/capture/capture-ts-events.mts.
    const std::vector<StreamingJsonCase> cases{
        // Complete JSON passes through exactly.
        {R"({"q":"x"})", R"({"q":"x"})"},
        // A trailing key with no value is dropped, not completed to null.
        {R"({"q":)", R"({})"},
        {R"({"q": )", R"({})"},
        {R"({"a":1,"b":})", R"({"a":1})"},
        // Unterminated values and containers recover the parsed prefix.
        {R"({"q": "x)", R"({"q":"x"})"},
        {R"({"q": "x",)", R"({"q":"x"})"},
        {R"({"a": {"b":)", R"({"a":{}})"},
        {R"({"q": "x", "r": [1, 2)", R"({"q":"x","r":[1,2]})"},
        {R"({"a": [1, {"b": 2},)", R"({"a":[1,{"b":2}]})"},
        {R"([)", R"([])"},
        {R"(["a",)", R"(["a"])"},
        {R"(["a", "b")", R"(["a","b"])"},
        // Empty input and non-recoverable input fall back to {}.
        {"", R"({})"},
        {"  ", R"({})"},
        {R"({"q":, )", R"({})"},
    };
    for (const auto& test_case : cases) {
        CHECK(parse(test_case.input) == test_case.expected);
    }
}

TEST_CASE(
    "streaming tool arguments repair malformed escapes like pi repairJson",
    "[ai][api][issue370]") {
    // A raw tab inside a string and an invalid `\H` escape are repaired by
    // pi's `repairJson` before the tolerant parse (kimi fixture case). The
    // parsed values are asserted directly; glaze re-escapes them on write.
    const auto value = ai::api::parse_streaming_json(
        R"({"path":"A\H","text":"col1	col2"})");
    REQUIRE(value.holds<util::JsonValue::object_t>());
    CHECK(value.at("path").get_string() == "A\\H");
    CHECK(value.at("text").get_string() == "col1\tcol2");
}
