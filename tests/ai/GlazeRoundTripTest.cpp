// AI-side Glaze case: the shared Glaze reader's nesting limit. The AI context
// JSON surface that lived in `src/ai/glaze/AiJson.hpp` was removed with #652,
// and the pi message JSON mapping is owned and tested by the session module
// (`SessionMessageJsonTest`, ADR 0056).

#include "support/Json.hpp"
#include "support/JsonGlaze.hpp"

#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cch;

TEST_CASE("Glaze accepts its nesting limit and rejects the next level", "[ai][u2][glaze][compat-pi]") {
    constexpr std::size_t kMaximumDepth = 256;
    std::string accepted_json(kMaximumDepth, '[');
    accepted_json += '0';
    accepted_json.append(kMaximumDepth, ']');
    REQUIRE(support::read_json<glz::generic>(accepted_json));

    std::string rejected_json(kMaximumDepth + 1, '[');
    rejected_json += '0';
    rejected_json.append(kMaximumDepth + 1, ']');
    const auto parsed = support::read_json<glz::generic>(rejected_json);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == support::ErrorCode::JsonParse);
}
