#pragma once

#include "../../../include/cch/ai/Tool.hpp"
#include "../../util/Json.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

namespace cch::ai::glaze {

struct ToolParametersDto {
    std::string type;
    std::optional<std::string> description;
    std::optional<std::map<std::string, ToolParametersDto>> properties;
    std::optional<std::vector<std::string>> required;
    std::optional<bool> additionalProperties;
    // std::optional<ToolParametersDto> creates direct recursion Glaze cannot handle.
    // glz::generic stores the already-serialized items schema as a single JSON object
    // (or is nullopt when there is no items schema), so Glaze reflects a plain nullable.
    std::optional<glz::generic> items;
};

struct FunctionToolDto {
    std::string name;
    std::string description;
    ToolParametersDto parameters;
};

[[nodiscard]] inline std::string schema_type_to_string(JsonSchemaType type) {
    switch (type) {
    case JsonSchemaType::Object:
        return "object";
    case JsonSchemaType::String:
        return "string";
    case JsonSchemaType::Integer:
        return "integer";
    case JsonSchemaType::Number:
        return "number";
    case JsonSchemaType::Boolean:
        return "boolean";
    case JsonSchemaType::Array:
        return "array";
    case JsonSchemaType::Null:
        return "null";
    }
    return "object";
}

[[nodiscard]] inline std::optional<JsonSchemaType> schema_type_from_string(const std::string& value) {
    if (value == "object") {
        return JsonSchemaType::Object;
    }
    if (value == "string") {
        return JsonSchemaType::String;
    }
    if (value == "integer") {
        return JsonSchemaType::Integer;
    }
    if (value == "number") {
        return JsonSchemaType::Number;
    }
    if (value == "boolean") {
        return JsonSchemaType::Boolean;
    }
    if (value == "array") {
        return JsonSchemaType::Array;
    }
    if (value == "null") {
        return JsonSchemaType::Null;
    }
    return std::nullopt;
}

[[nodiscard]] inline glz::generic tool_parameters_to_generic(const ToolParametersDto& dto) {
    glz::generic obj = glz::generic::object_t{};
    obj["type"] = dto.type;
    if (dto.description) {
        obj["description"] = *dto.description;
    }
    if (dto.properties) {
        glz::generic props = glz::generic::object_t{};
        for (const auto& [name, property] : *dto.properties) {
            props[name] = tool_parameters_to_generic(property);
        }
        obj["properties"] = std::move(props);
    }
    if (dto.required) {
        glz::generic::array_t req;
        for (const auto& name : *dto.required) {
            req.emplace_back(name);
        }
        obj["required"] = std::move(req);
    }
    if (dto.additionalProperties) {
        obj["additionalProperties"] = *dto.additionalProperties;
    }
    if (dto.items) {
        obj["items"] = *dto.items;
    }
    return obj;
}

[[nodiscard]] inline util::Expected<JsonSchema> json_schema_from_generic(const glz::generic& value) {
    if (!value.is_object()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "tool schema items is not an object",
            "expected JSON object for items schema"));
    }

    JsonSchema schema;
    const auto& obj = value.get_object();

    if (auto it = obj.find("type"); it != obj.end() && it->second.is_string()) {
        auto type = schema_type_from_string(it->second.get_string());
        if (!type) {
            return std::unexpected(util::make_error(
                util::ErrorCode::JsonParse,
                "unknown JSON schema type",
                "unknown schema type '" + it->second.get_string() + "'"));
        }
        schema.type = *type;
    }

    if (auto it = obj.find("description"); it != obj.end() && it->second.is_string()) {
        schema.description = it->second.get_string();
    }

    if (auto it = obj.find("properties"); it != obj.end() && it->second.is_object()) {
        for (const auto& [name, property] : it->second.get_object()) {
            auto converted = json_schema_from_generic(property);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            schema.properties.emplace(name, std::move(*converted));
        }
    }

    if (auto it = obj.find("required"); it != obj.end() && it->second.is_array()) {
        for (const auto& entry : it->second.get_array()) {
            if (entry.is_string()) {
                schema.required.push_back(entry.get_string());
            }
        }
    }

    if (auto it = obj.find("additionalProperties"); it != obj.end() && it->second.is_boolean()) {
        schema.additional_properties = it->second.get_boolean();
    }

    if (auto it = obj.find("items"); it != obj.end()) {
        auto converted = json_schema_from_generic(it->second);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        schema.items = std::make_shared<JsonSchema>(std::move(*converted));
    }

    return schema;
}

[[nodiscard]] inline ToolParametersDto to_tool_parameters_dto(const JsonSchema& schema) {
    ToolParametersDto dto;
    dto.type = schema_type_to_string(schema.type);
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
    if (schema.items) {
        dto.items = tool_parameters_to_generic(to_tool_parameters_dto(*schema.items));
    }
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
    if (dto.items) {
        auto converted = json_schema_from_generic(*dto.items);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        schema.items = std::make_shared<JsonSchema>(std::move(*converted));
    }
    return schema;
}

[[nodiscard]] inline FunctionToolDto to_function_tool_dto(const Tool& tool) {
    return FunctionToolDto{tool.name, tool.description, to_tool_parameters_dto(tool.parameters)};
}

[[nodiscard]] inline util::Expected<std::string> write_tool_parameters_json(const JsonSchema& schema) {
    return util::write_json(to_tool_parameters_dto(schema));
}

[[nodiscard]] inline util::Expected<std::string> write_function_tool_json(const Tool& tool) {
    return util::write_json(to_function_tool_dto(tool));
}

} // namespace cch::ai::glaze
