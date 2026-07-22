#include "../../third_party/catch2/catch_test_macros.hpp"

#include "ComplexToolSchemaFixture.hpp"
#include "ai/glaze/ToolDtos.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "../../include/cch/util/JsonValue.hpp"
#include "util/Json.hpp"

#include <string>
#include <type_traits>

using namespace cch;

namespace {

util::JsonValue complex_contract() {
    auto parsed = util::read_json<util::JsonValue>(test::kComplexToolArgumentContract);
    REQUIRE(parsed);
    return std::move(*parsed);
}

std::string canonical_json(const util::JsonValue& value) {
    auto serialized = util::write_json(value);
    REQUIRE(serialized);
    return std::move(*serialized);
}

} // namespace

TEST_CASE("tool parameters are the passive project JSON value", "[ai][u2][tool][issue24]") {
    static_assert(std::is_same_v<decltype(ai::Tool::parameters), util::JsonValue>);

    ai::Tool object_tool{"object_contract", "Object contract", complex_contract()};
    ai::Tool boolean_tool{"boolean_contract", "Boolean contract", util::JsonValue{false}};

    CHECK(object_tool.parameters.holds<util::JsonValue::object_t>());
    CHECK(boolean_tool.parameters.holds<bool>());
    CHECK_FALSE(boolean_tool.parameters.get_boolean());
}

TEST_CASE("function tool serialization preserves arbitrary JSON Schema values", "[ai][u2][tool][issue24]") {
    const auto expected = complex_contract();
    const ai::Tool tool{"complete_contract", "Complete contract", expected};

    auto json = ai::glaze::write_function_tool_json(tool);
    REQUIRE(json);
    auto parsed = util::read_json<util::JsonValue>(*json);
    REQUIRE(parsed);

    CHECK(parsed->at("name").get_string() == "complete_contract");
    CHECK(parsed->at("description").get_string() == "Complete contract");
    CHECK(canonical_json(parsed->at("parameters")) == canonical_json(expected));
}

TEST_CASE("function tool serialization preserves boolean JSON Schemas", "[ai][u2][tool][issue24]") {
    const ai::Tool tool{"disabled_contract", "Reject every argument", util::JsonValue{false}};

    auto json = ai::glaze::write_function_tool_json(tool);
    REQUIRE(json);
    auto parsed = util::read_json<util::JsonValue>(*json);
    REQUIRE(parsed);

    REQUIRE(parsed->at("parameters").holds<bool>());
    CHECK_FALSE(parsed->at("parameters").get_boolean());
}
