#pragma once

#include "../../include/cch/util/JsonValue.hpp"

namespace cch::test {

[[nodiscard]] inline util::JsonValue empty_object_tool_argument_contract() {
    return util::JsonValue::object_t{
        {"type", "object"},
        {"additionalProperties", false},
    };
}

[[nodiscard]] inline util::JsonValue permissive_object_tool_argument_contract() {
    return util::JsonValue::object_t{{"type", "object"}};
}

[[nodiscard]] inline util::JsonValue path_tool_argument_contract() {
    return util::JsonValue::object_t{
        {"type", "object"},
        {"properties", util::JsonValue::object_t{
            {"path", util::JsonValue::object_t{
                {"type", "string"},
                {"description", "file path"},
            }},
        }},
        {"required", util::JsonValue::array_t{"path"}},
        {"additionalProperties", false},
    };
}

} // namespace cch::test
