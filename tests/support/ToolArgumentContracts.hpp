#pragma once

#include <cch/support/JsonValue.hpp>

namespace cch::test {

[[nodiscard]] inline support::JsonValue empty_object_tool_argument_contract() {
    return support::JsonValue::object_t{
        {"type", "object"},
        {"additionalProperties", false},
    };
}

[[nodiscard]] inline support::JsonValue permissive_object_tool_argument_contract() {
    return support::JsonValue::object_t{{"type", "object"}};
}

[[nodiscard]] inline support::JsonValue integer_value_tool_argument_contract() {
    return support::JsonValue::object_t{
        {"type", "object"},
        {"properties", support::JsonValue::object_t{
            {"value", support::JsonValue::object_t{{"type", "integer"}}},
        }},
        {"required", support::JsonValue::array_t{"value"}},
        {"additionalProperties", false},
    };
}

[[nodiscard]] inline support::JsonValue path_tool_argument_contract() {
    return support::JsonValue::object_t{
        {"type", "object"},
        {"properties", support::JsonValue::object_t{
            {"path", support::JsonValue::object_t{
                {"type", "string"},
                {"description", "file path"},
            }},
        }},
        {"required", support::JsonValue::array_t{"path"}},
        {"additionalProperties", false},
    };
}

} // namespace cch::test
