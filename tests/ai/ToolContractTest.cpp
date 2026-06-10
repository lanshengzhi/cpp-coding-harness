#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/Tool.hpp"
#include "../../src/ai/Tool.hpp"
#include "../../src/util/JsonSchema.hpp"

using namespace cch;

TEST_CASE("tool definitions preserve name description and schema through AI adapter", "[ai][u2]") {
    boost::json::object properties;
    properties["path"] = util::string_property("file path");
    properties["limit"] = util::integer_property("line limit");
    agent::ToolDefinition legacy{"read_file", "Read a workspace file", util::object_schema(std::move(properties), {"path"})};

    auto contract = agent::to_ai_tool_definition(legacy);

    CHECK(contract.name == "read_file");
    CHECK(contract.description == "Read a workspace file");
    REQUIRE(contract.parameters.at("properties").is_object());
    CHECK(contract.parameters.at("properties").as_object().contains("path"));
    CHECK(contract.parameters.at("required").as_array().front().as_string() == "path");

    auto round_trip = agent::tool_definition_from_ai(contract);
    CHECK(round_trip.name == legacy.name);
    CHECK(round_trip.description == legacy.description);
    CHECK(boost::json::serialize(round_trip.parameters) == boost::json::serialize(legacy.parameters));
}
