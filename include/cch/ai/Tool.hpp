#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai {

enum class JsonSchemaType {
    Object,
    String,
    Integer,
    Number,
    Boolean,
    Array,
    Null,
};

[[nodiscard]] inline std::string to_string(JsonSchemaType type) {
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

struct JsonSchema {
    JsonSchemaType type{JsonSchemaType::Object};
    std::optional<std::string> description;
    std::map<std::string, JsonSchema> properties;
    std::vector<std::string> required;
    std::optional<bool> additional_properties;

    [[nodiscard]] static JsonSchema object(
        std::map<std::string, JsonSchema> properties = {},
        std::vector<std::string> required = {},
        std::optional<std::string> description = std::nullopt,
        std::optional<bool> additional_properties = false) {
        JsonSchema schema;
        schema.type = JsonSchemaType::Object;
        schema.description = std::move(description);
        schema.properties = std::move(properties);
        schema.required = std::move(required);
        schema.additional_properties = additional_properties;
        return schema;
    }

    [[nodiscard]] static JsonSchema string(std::optional<std::string> description = std::nullopt) {
        JsonSchema schema;
        schema.type = JsonSchemaType::String;
        schema.description = std::move(description);
        return schema;
    }

    [[nodiscard]] static JsonSchema integer(std::optional<std::string> description = std::nullopt) {
        JsonSchema schema;
        schema.type = JsonSchemaType::Integer;
        schema.description = std::move(description);
        return schema;
    }

    [[nodiscard]] static JsonSchema boolean(std::optional<std::string> description = std::nullopt) {
        JsonSchema schema;
        schema.type = JsonSchemaType::Boolean;
        schema.description = std::move(description);
        return schema;
    }
};

struct Tool {
    std::string name;
    std::string description;
    JsonSchema parameters;
};

} // namespace cch::ai
