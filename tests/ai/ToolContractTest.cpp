#include "support/ComplexToolSchemaFixture.hpp"
#include "support/Json.hpp"
#include <cch/ai/Tool.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace cch;

namespace {

support::JsonValue complex_contract() {
    auto parsed = support::read_json(tests::kComplexToolArgumentContract);
    REQUIRE(parsed);
    return std::move(*parsed);
}

} // namespace

TEST_CASE("tool parameters are the passive project JSON value", "[ai][u2][tool][issue24][compat-pi]") {
    static_assert(std::is_same_v<decltype(ai::Tool::parameters), support::JsonValue>);

    ai::Tool object_tool{"object_contract", "Object contract", complex_contract()};
    ai::Tool boolean_tool{"boolean_contract", "Boolean contract", support::JsonValue{false}};

    CHECK(object_tool.parameters.holds<support::JsonValue::object_t>());
    CHECK(boolean_tool.parameters.holds<bool>());
    CHECK_FALSE(boolean_tool.parameters.get_boolean());
}
