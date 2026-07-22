#pragma once

#include "../../../include/cch/ai/Tool.hpp"
#include "../../util/Json.hpp"

#include <string>

#include <glaze/glaze.hpp>

namespace cch::ai::glaze {

struct FunctionToolDto {
    std::string name;
    std::string description;
    glz::generic parameters;
};

[[nodiscard]] inline FunctionToolDto to_function_tool_dto(const Tool& tool) {
    return FunctionToolDto{
        tool.name,
        tool.description,
        util::json_to_glaze(tool.parameters),
    };
}

[[nodiscard]] inline Tool tool_from_function_tool_dto(const FunctionToolDto& dto) {
    return Tool{
        dto.name,
        dto.description,
        util::json_from_glaze(dto.parameters),
    };
}

[[nodiscard]] inline util::Expected<std::string> write_function_tool_json(const Tool& tool) {
    return util::write_json(to_function_tool_dto(tool));
}

} // namespace cch::ai::glaze
