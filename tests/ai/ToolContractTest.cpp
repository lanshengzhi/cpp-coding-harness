#include "../../third_party/catch2/catch_test_macros.hpp"

#include "ai/glaze/ToolSchemaDtos.hpp"
#include "../../include/cch/util/Error.hpp"
#include "util/Json.hpp"

#include <memory>
#include <string>
#include <type_traits>

using namespace cch;

TEST_CASE("tool definitions serialize function schema DTOs without Boost JSON", "[ai][u2][tool]") {
    ai::Tool read_file;
    read_file.name = "read_file";
    read_file.description = "Read a workspace file";
    read_file.parameters = ai::JsonSchema::object(
        {
            {"path", ai::JsonSchema::string("file path")},
            {"limit", ai::JsonSchema::integer("line limit")},
        },
        {"path"},
        std::nullopt,
        false);

    auto json = ai::glaze::write_function_tool_json(read_file);
    REQUIRE(json);
    CHECK(json->find(R"("name":"read_file")") != std::string::npos);
    CHECK(json->find(R"("parameters")") != std::string::npos);
    CHECK(json->find(R"("additionalProperties":false)") != std::string::npos);

    auto parsed = util::read_json<util::JsonValue>(*json);
    REQUIRE(parsed);
    const auto& function = parsed->get<util::JsonValue::object_t>();
    CHECK(function.at("name").get_string() == "read_file");
    CHECK(function.at("description").get_string() == "Read a workspace file");

    const auto& parameters = function.at("parameters").get<util::JsonValue::object_t>();
    CHECK(parameters.at("type").get_string() == "object");
    CHECK(parameters.at("additionalProperties").get<bool>() == false);

    const auto& properties = parameters.at("properties").get<util::JsonValue::object_t>();
    CHECK(properties.at("path").get<util::JsonValue::object_t>().at("type").get_string() == "string");
    CHECK(properties.at("path").get<util::JsonValue::object_t>().at("description").get_string() == "file path");
    CHECK(properties.at("limit").get<util::JsonValue::object_t>().at("type").get_string() == "integer");

    const auto& required = parameters.at("required").get<util::JsonValue::array_t>();
    REQUIRE(required.size() == 1);
    CHECK(required[0].get_string() == "path");
}

TEST_CASE("tool schema DTOs round-trip into typed schema contracts", "[ai][u2][tool]") {
    ai::Tool original;
    original.name = "write_file";
    original.description = "Write a workspace file";
    original.parameters = ai::JsonSchema::object(
        {{"path", ai::JsonSchema::string("file path")}, {"content", ai::JsonSchema::string("new content")}},
        {"path", "content"});

    auto dto = ai::glaze::to_function_tool_dto(original);
    auto converted = ai::glaze::schema_from_tool_parameters_dto(dto.parameters);
    REQUIRE(converted);

    CHECK(converted->type == ai::JsonSchemaType::Object);
    CHECK(converted->properties.at("path").type == ai::JsonSchemaType::String);
    CHECK(converted->properties.at("content").type == ai::JsonSchemaType::String);
    REQUIRE(converted->properties.at("content").description);
    CHECK(*converted->properties.at("content").description == "new content");
    REQUIRE(converted->required.size() == 2);
    CHECK(converted->required[0] == "path");
    CHECK(converted->required[1] == "content");
}

TEST_CASE("array schemas clone immutable item schemas", "[ai][u2][tool]") {
    auto item = std::make_shared<ai::JsonSchema>(ai::JsonSchema::string("item"));
    auto array = ai::JsonSchema::array(item);
    static_assert(std::is_same_v<decltype(array.items), std::shared_ptr<const ai::JsonSchema>>);

    item->type = ai::JsonSchemaType::Integer;

    REQUIRE(array.items);
    CHECK(array.items->type == ai::JsonSchemaType::String);
    auto copied = array;
    REQUIRE(copied.items);
    CHECK(copied.items->type == ai::JsonSchemaType::String);
}
