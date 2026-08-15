#include <cch/support/Error.hpp>
#include "support/Json.hpp"
#include "support/JsonGlaze.hpp"

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string>

namespace {

struct ProbePayload {
    int count{};
    std::string label;
};

} // namespace

TEST_CASE("std expected is the project failure carrier", "[support][expected][u1]") {
    cch::support::Expected<int> ok = 42;
    REQUIRE(ok);
    CHECK(*ok == 42);

    cch::support::Expected<int> failed = std::unexpected(cch::support::make_error(
        cch::support::ErrorCode::Validation,
        "invalid input",
        "count must be positive"));

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == cch::support::ErrorCode::Validation);
    CHECK(cch::support::to_string(failed.error().code) == "validation");
    CHECK(failed.error().message == "invalid input");
    CHECK(failed.error().detail == "count must be positive");
}

TEST_CASE("Glaze round-trip failures become typed project errors", "[support][glaze][u1]") {
    ProbePayload payload{7, "ok"};

    auto json = cch::support::write_json(payload);
    REQUIRE(json);
    CHECK(json->find("count") != std::string::npos);
    CHECK(json->find("label") != std::string::npos);

    auto parsed = cch::support::read_json<ProbePayload>(*json);
    REQUIRE(parsed);
    CHECK(parsed->count == 7);
    CHECK(parsed->label == "ok");

    auto malformed = cch::support::read_json<ProbePayload>(R"({"count":"not-an-int","label":"bad"})");
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == cch::support::ErrorCode::JsonParse);
    CHECK(cch::support::to_string(malformed.error().code) == "json_parse");
    CHECK_FALSE(malformed.error().message.empty());
    CHECK_FALSE(malformed.error().detail.empty());
    REQUIRE(malformed.error().context);
    CHECK(malformed.error().context->find("not-an-int") != std::string::npos);
}
