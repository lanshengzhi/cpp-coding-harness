#pragma once

#include "../Tool.hpp"
#include "../../util/Error.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai::glaze {

struct ToolParametersDto {
    std::string type;
    std::optional<std::string> description;
    std::optional<std::map<std::string, ToolParametersDto>> properties;
    std::optional<std::vector<std::string>> required;
    std::optional<bool> additionalProperties;
};

struct FunctionToolDto {
    std::string name;
    std::string description;
    ToolParametersDto parameters;
};

struct ProviderToolDto {
    std::string type{"function"};
    FunctionToolDto function;
};

struct ProviderToolCallFunctionDto {
    std::string name;
    std::string arguments;
};

struct ProviderToolCallDto {
    std::string id;
    std::string type{"function"};
    ProviderToolCallFunctionDto function;
};

[[nodiscard]] inline ToolParametersDto to_tool_parameters_dto(const JsonSchema& schema) {
    ToolParametersDto dto;
    dto.type = to_string(schema.type);
    dto.description = schema.description;
    if (!schema.properties.empty()) {
        std::map<std::string, ToolParametersDto> properties;
        for (const auto& [name, property] : schema.properties) {
            properties.emplace(name, to_tool_parameters_dto(property));
        }
        dto.properties = std::move(properties);
    }
    if (!schema.required.empty()) {
        dto.required = schema.required;
    }
    dto.additionalProperties = schema.additional_properties;
    return dto;
}

[[nodiscard]] inline util::Expected<JsonSchema> schema_from_tool_parameters_dto(const ToolParametersDto& dto) {
    auto type = schema_type_from_string(dto.type);
    if (!type) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "unknown JSON schema type",
            "unknown schema type '" + dto.type + "'"));
    }

    JsonSchema schema;
    schema.type = *type;
    schema.description = dto.description;
    schema.additional_properties = dto.additionalProperties;
    if (dto.properties) {
        for (const auto& [name, property] : *dto.properties) {
            auto converted = schema_from_tool_parameters_dto(property);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            schema.properties.emplace(name, std::move(*converted));
        }
    }
    if (dto.required) {
        schema.required = *dto.required;
    }
    return schema;
}

[[nodiscard]] inline FunctionToolDto to_function_tool_dto(const Tool& tool) {
    return FunctionToolDto{tool.name, tool.description, to_tool_parameters_dto(tool.parameters)};
}

[[nodiscard]] inline ProviderToolDto to_provider_tool_dto(const Tool& tool) {
    return ProviderToolDto{"function", to_function_tool_dto(tool)};
}

[[nodiscard]] inline util::Expected<std::string> write_tool_parameters_json(const JsonSchema& schema) {
    return util::write_json(to_tool_parameters_dto(schema));
}

[[nodiscard]] inline util::Expected<std::string> write_provider_tool_json(const Tool& tool) {
    return util::write_json(to_provider_tool_dto(tool));
}

} // namespace cch::ai::glaze
