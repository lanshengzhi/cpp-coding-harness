#pragma once

#include <cch/ai/Tool.hpp>
#include "support/JsonGlaze.hpp"

#include <string>

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
        support::json_to_glaze(tool.parameters),
    };
}

[[nodiscard]] inline Tool tool_from_function_tool_dto(const FunctionToolDto& dto) {
    return Tool{
        .name = dto.name,
        .description = dto.description,
        .parameters = support::json_from_glaze(dto.parameters),
    };
}

[[nodiscard]] inline support::Expected<std::string> write_function_tool_json(const Tool& tool) {
    return support::write_json(to_function_tool_dto(tool));
}

} // namespace cch::ai::glaze
