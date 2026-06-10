#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/ai/glaze/AiJson.hpp>
#include <cch/util/Error.hpp>

#include <glaze/glaze.hpp>

#include <string>
#include <variant>

using namespace cch;

TEST_CASE("tool-result message round-trips linkage details and error state", "[ai][u2][glaze]") {
    auto details = util::read_json<glz::generic>(R"({"exitCode":2,"stderr":"denied"})");
    REQUIRE(details);

    ai::ToolResultMessage result;
    result.tool_call_id = "call-1";
    result.tool_name = "read_file";
    result.content.emplace_back(ai::TextContent{"could not read", std::nullopt});
    result.content.emplace_back(ai::ImageContent{"ZmFrZQ==", "image/png"});
    result.details = *details;
    result.is_error = true;
    result.timestamp = 1718000000456;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{result});
    REQUIRE(json);
    CHECK(json->find(R"("role":"toolResult")") != std::string::npos);
    CHECK(json->find(R"("toolCallId":"call-1")") != std::string::npos);
    CHECK(json->find(R"("isError":true)") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(*parsed));
    const auto& round_trip = std::get<ai::ToolResultMessage>(*parsed);
    CHECK(round_trip.tool_call_id == "call-1");
    CHECK(round_trip.tool_name == "read_file");
    CHECK(round_trip.is_error);
    CHECK(round_trip.timestamp == 1718000000456);
    REQUIRE(round_trip.content.size() == 2);
    REQUIRE(std::holds_alternative<ai::TextContent>(round_trip.content[0]));
    CHECK(std::get<ai::TextContent>(round_trip.content[0]).text == "could not read");
    REQUIRE(std::holds_alternative<ai::ImageContent>(round_trip.content[1]));
    CHECK(std::get<ai::ImageContent>(round_trip.content[1]).mime_type == "image/png");
    REQUIRE(round_trip.details);
    const auto& detail_object = round_trip.details->get<glz::generic::object_t>();
    CHECK(static_cast<int>(detail_object.at("exitCode").get<double>()) == 2);
    CHECK(detail_object.at("stderr").get_string() == "denied");
}

