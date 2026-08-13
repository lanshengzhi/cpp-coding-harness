#include <cch/util/Error.hpp>
#include "util/Json.hpp"
#include "util/JsonGlaze.hpp"

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string>

namespace {

struct ProbePayload {
    int count{};
    std::string label;
};

} // namespace

TEST_CASE("std expected is the project failure carrier", "[util][expected][u1]") {
    cch::util::Expected<int> ok = 42;
    REQUIRE(ok);
    CHECK(*ok == 42);

    cch::util::Expected<int> failed = std::unexpected(cch::util::make_error(
        cch::util::ErrorCode::Validation,
        "invalid input",
        "count must be positive"));

    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == cch::util::ErrorCode::Validation);
    CHECK(cch::util::to_string(failed.error().code) == "validation");
    CHECK(failed.error().message == "invalid input");
    CHECK(failed.error().detail == "count must be positive");
}

TEST_CASE("Glaze round-trip failures become typed project errors", "[util][glaze][u1]") {
    ProbePayload payload{7, "ok"};

    auto json = cch::util::write_json(payload);
    REQUIRE(json);
    CHECK(json->find("count") != std::string::npos);
    CHECK(json->find("label") != std::string::npos);

    auto parsed = cch::util::read_json<ProbePayload>(*json);
    REQUIRE(parsed);
    CHECK(parsed->count == 7);
    CHECK(parsed->label == "ok");

    auto malformed = cch::util::read_json<ProbePayload>(R"({"count":"not-an-int","label":"bad"})");
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == cch::util::ErrorCode::JsonParse);
    CHECK(cch::util::to_string(malformed.error().code) == "json_parse");
    CHECK_FALSE(malformed.error().message.empty());
    CHECK_FALSE(malformed.error().detail.empty());
    REQUIRE(malformed.error().context);
    CHECK(malformed.error().context->find("not-an-int") != std::string::npos);
}
