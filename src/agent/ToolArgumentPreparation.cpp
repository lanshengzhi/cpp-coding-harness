#include "ToolArgumentPreparation.hpp"

#include "util/Json.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::agent {
namespace {

constexpr std::size_t kMaxSchemaDepth = 64;
constexpr std::size_t kMaxSchemaNodes = 2048;
constexpr std::size_t kMaxValidationFailures = 8;

enum class JsonType {
    Null,
    Boolean,
    Number,
    Integer,
    String,
    Array,
    Object,
};

enum class AdditionalPropertiesMode {
    Allow,
    Deny,
    Schema,
};

struct CompiledSchema {
    bool reject_all{false};
    std::vector<JsonType> types;
    std::optional<util::JsonValue::array_t> enum_values;
    std::optional<util::JsonValue> constant;
    std::map<std::string, CompiledSchema> properties;
    std::vector<std::string> required;
    AdditionalPropertiesMode additional_properties{AdditionalPropertiesMode::Allow};
    std::unique_ptr<CompiledSchema> additional_properties_schema;
};

struct CompilationState {
    std::size_t nodes{0};
};

struct ValidationFailure {
    std::string location;
    std::string reason;
};

[[nodiscard]] util::Error preparation_error(
    const std::string& tool_name,
    std::string reason,
    util::ErrorCode code = util::ErrorCode::Validation) {
    return util::make_error(
        code,
        "tool argument preparation failed",
        bounded_tool_argument_diagnostic(
            "Tool Argument Contract preparation failed at root for tool \"" +
            bounded_tool_argument_component(tool_name, 256) + "\": " +
            bounded_tool_argument_component(std::move(reason), 3500)));
}

[[nodiscard]] std::string child_path(std::string_view base, std::string_view key) {
    if (base == "root") {
        return std::string(key);
    }
    std::string result(base);
    result.push_back('.');
    result.append(key);
    return result;
}

[[nodiscard]] std::string instance_child_path(std::string_view base, std::string_view key) {
    std::string result = base == "root" ? std::string{} : std::string(base);
    result.push_back('/');
    for (const char character : key) {
        if (character == '~') {
            result += "~0";
        } else if (character == '/') {
            result += "~1";
        } else {
            result.push_back(character);
        }
    }
    return result;
}

[[nodiscard]] bool json_equal(const util::JsonValue& left, const util::JsonValue& right) {
    if (left.data.index() != right.data.index()) {
        return false;
    }
    if (left.holds<util::JsonValue::null_t>()) {
        return true;
    }
    if (const auto* number = left.get_if<double>()) {
        return *number == right.get<double>();
    }
    if (const auto* text = left.get_if<std::string>()) {
        return *text == right.get<std::string>();
    }
    if (const auto* flag = left.get_if<bool>()) {
        return *flag == right.get<bool>();
    }
    if (const auto* array = left.get_if<util::JsonValue::array_t>()) {
        const auto& other = right.get<util::JsonValue::array_t>();
        return array->size() == other.size() &&
               std::equal(array->begin(), array->end(), other.begin(), json_equal);
    }

    const auto& object = left.get<util::JsonValue::object_t>();
    const auto& other = right.get<util::JsonValue::object_t>();
    if (object.size() != other.size()) {
        return false;
    }
    auto left_it = object.begin();
    auto right_it = other.begin();
    for (; left_it != object.end(); ++left_it, ++right_it) {
        if (left_it->first != right_it->first || !json_equal(left_it->second, right_it->second)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<JsonType> json_type_from_name(std::string_view name) {
    if (name == "null") {
        return JsonType::Null;
    }
    if (name == "boolean") {
        return JsonType::Boolean;
    }
    if (name == "number") {
        return JsonType::Number;
    }
    if (name == "integer") {
        return JsonType::Integer;
    }
    if (name == "string") {
        return JsonType::String;
    }
    if (name == "array") {
        return JsonType::Array;
    }
    if (name == "object") {
        return JsonType::Object;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_known_unsupported_keyword(std::string_view keyword) {
    // These assertions and declarations are losslessly transportable, but the
    // foundational executable profile cannot enforce them yet. Failing closed
    // keeps a recognized contract from becoming annotation-only by accident.
    constexpr std::array<std::string_view, 39> unsupported{
        "$dynamicRef", "$recursiveRef", "$ref", "$schema", "$vocabulary",
        "allOf", "anyOf", "contains", "dependentRequired", "dependentSchemas", "else",
        "exclusiveMaximum", "exclusiveMinimum", "format", "if", "items",
        "maxContains", "maxItems", "maxLength", "maxProperties", "maximum",
        "minContains", "minItems", "minLength", "minProperties", "minimum",
        "multipleOf", "not", "oneOf", "pattern", "patternProperties",
        "prefixItems", "propertyNames", "then", "unevaluatedItems",
        "unevaluatedProperties", "uniqueItems", "additionalItems", "dependencies",
    };
    return std::find(unsupported.begin(), unsupported.end(), keyword) != unsupported.end();
}

[[nodiscard]] util::Expected<std::vector<JsonType>> compile_types(
    const util::JsonValue& value,
    const std::string& schema_path) {
    std::vector<JsonType> result;
    auto add_type = [&](const util::JsonValue& candidate) -> util::ExpectedVoid {
        const auto* type_name = candidate.get_if<std::string>();
        if (type_name == nullptr) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                schema_path + " must contain only JSON type names"));
        }
        const auto type = json_type_from_name(*type_name);
        if (!type) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                schema_path + " contains unsupported JSON type \"" + *type_name + "\""));
        }
        if (std::find(result.begin(), result.end(), *type) != result.end()) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                schema_path + " contains a duplicate JSON type"));
        }
        result.push_back(*type);
        return {};
    };

    if (value.holds<std::string>()) {
        if (auto added = add_type(value); !added) {
            return std::unexpected(added.error());
        }
        return result;
    }
    const auto* values = value.get_if<util::JsonValue::array_t>();
    if (values == nullptr || values->empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid Tool Argument Contract",
            schema_path + " must be a JSON type name or a non-empty array of unique type names"));
    }
    for (const auto& candidate : *values) {
        if (auto added = add_type(candidate); !added) {
            return std::unexpected(added.error());
        }
    }
    return result;
}

[[nodiscard]] util::Expected<CompiledSchema> compile_schema(
    const util::JsonValue& schema,
    std::string schema_path,
    std::size_t depth,
    CompilationState& state) {
    if (depth > kMaxSchemaDepth || ++state.nodes > kMaxSchemaNodes) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid Tool Argument Contract",
            schema_path + " exceeds the executable schema complexity limit"));
    }

    if (const auto* boolean_schema = schema.get_if<bool>()) {
        CompiledSchema compiled;
        compiled.reject_all = !*boolean_schema;
        return compiled;
    }
    const auto* object = schema.get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid Tool Argument Contract",
            schema_path + " must be an object or boolean schema"));
    }

    for (const auto& [keyword, _] : *object) {
        if (is_known_unsupported_keyword(keyword)) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "unsupported Tool Argument Contract",
                child_path(schema_path, keyword) +
                    " is a recognized executable construct outside the foundational profile"));
        }
    }

    CompiledSchema compiled;
    if (const auto it = object->find("type"); it != object->end()) {
        auto types = compile_types(it->second, child_path(schema_path, "type"));
        if (!types) {
            return std::unexpected(types.error());
        }
        compiled.types = std::move(*types);
    }

    if (const auto it = object->find("enum"); it != object->end()) {
        const auto* values = it->second.get_if<util::JsonValue::array_t>();
        if (values == nullptr || values->empty()) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                child_path(schema_path, "enum") + " must be a non-empty array"));
        }
        for (std::size_t left = 0; left < values->size(); ++left) {
            for (std::size_t right = left + 1; right < values->size(); ++right) {
                if (json_equal((*values)[left], (*values)[right])) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Validation,
                        "invalid Tool Argument Contract",
                        child_path(schema_path, "enum") + " values must be unique"));
                }
            }
        }
        compiled.enum_values = *values;
    }

    if (const auto it = object->find("const"); it != object->end()) {
        compiled.constant = it->second;
    }

    if (const auto it = object->find("properties"); it != object->end()) {
        const auto* properties = it->second.get_if<util::JsonValue::object_t>();
        if (properties == nullptr) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                child_path(schema_path, "properties") + " must be an object"));
        }
        for (const auto& [name, property_schema] : *properties) {
            auto nested = compile_schema(
                property_schema,
                child_path(child_path(schema_path, "properties"), name),
                depth + 1,
                state);
            if (!nested) {
                return std::unexpected(nested.error());
            }
            compiled.properties.emplace(name, std::move(*nested));
        }
    }

    if (const auto it = object->find("required"); it != object->end()) {
        const auto* required = it->second.get_if<util::JsonValue::array_t>();
        if (required == nullptr) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                child_path(schema_path, "required") + " must be an array of unique property names"));
        }
        std::set<std::string> seen;
        for (const auto& entry : *required) {
            const auto* name = entry.get_if<std::string>();
            if (name == nullptr || !seen.insert(*name).second) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    "invalid Tool Argument Contract",
                    child_path(schema_path, "required") +
                        " must contain unique string property names"));
            }
            compiled.required.push_back(*name);
        }
    }

    if (const auto it = object->find("additionalProperties"); it != object->end()) {
        if (const auto* allow = it->second.get_if<bool>()) {
            compiled.additional_properties = *allow ? AdditionalPropertiesMode::Allow
                                                    : AdditionalPropertiesMode::Deny;
        } else {
            auto nested = compile_schema(
                it->second,
                child_path(schema_path, "additionalProperties"),
                depth + 1,
                state);
            if (!nested) {
                return std::unexpected(nested.error());
            }
            compiled.additional_properties = AdditionalPropertiesMode::Schema;
            compiled.additional_properties_schema =
                std::make_unique<CompiledSchema>(std::move(*nested));
        }
    }

    return compiled;
}

[[nodiscard]] bool matches_type(const util::JsonValue& value, JsonType type) {
    switch (type) {
    case JsonType::Null:
        return value.holds<util::JsonValue::null_t>();
    case JsonType::Boolean:
        return value.holds<bool>();
    case JsonType::Number:
        return value.holds<double>() && std::isfinite(value.get<double>());
    case JsonType::Integer:
        return value.holds<double>() && std::isfinite(value.get<double>()) &&
               std::trunc(value.get<double>()) == value.get<double>();
    case JsonType::String:
        return value.holds<std::string>();
    case JsonType::Array:
        return value.holds<util::JsonValue::array_t>();
    case JsonType::Object:
        return value.holds<util::JsonValue::object_t>();
    }
    return false;
}

[[nodiscard]] std::size_t ecmascript_whitespace_prefix(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    const unsigned char first = static_cast<unsigned char>(text.front());
    if ((first >= 0x09 && first <= 0x0d) || first == 0x20) {
        return 1;
    }
    constexpr std::array<std::string_view, 8> multibyte_whitespace{
        "\xc2\xa0",
        "\xe1\x9a\x80",
        "\xe2\x80\xa8",
        "\xe2\x80\xa9",
        "\xe2\x80\xaf",
        "\xe2\x81\x9f",
        "\xe3\x80\x80",
        "\xef\xbb\xbf",
    };
    for (const auto whitespace : multibyte_whitespace) {
        if (text.starts_with(whitespace)) {
            return whitespace.size();
        }
    }
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xe2 &&
        static_cast<unsigned char>(text[1]) == 0x80 &&
        static_cast<unsigned char>(text[2]) >= 0x80 &&
        static_cast<unsigned char>(text[2]) <= 0x8a) {
        return 3;
    }
    return 0;
}

[[nodiscard]] std::string_view trim_ecmascript_whitespace(std::string_view text) {
    while (const auto prefix = ecmascript_whitespace_prefix(text)) {
        text.remove_prefix(prefix);
    }
    while (!text.empty()) {
        bool removed = false;
        const std::size_t suffix_window = std::min<std::size_t>(text.size(), 3);
        for (std::size_t length = 1; length <= suffix_window; ++length) {
            const auto suffix = text.substr(text.size() - length);
            if (ecmascript_whitespace_prefix(suffix) == length) {
                text.remove_suffix(length);
                removed = true;
                break;
            }
        }
        if (!removed) {
            break;
        }
    }
    return text;
}

[[nodiscard]] std::optional<double> parse_finite_number(std::string_view text) {
    text = trim_ecmascript_whitespace(text);
    if (text.empty()) {
        return std::nullopt;
    }
    std::string candidate(text);
    if (candidate.size() > 2 && candidate[0] == '0' &&
        (candidate[1] == 'b' || candidate[1] == 'B' ||
         candidate[1] == 'o' || candidate[1] == 'O' ||
         candidate[1] == 'x' || candidate[1] == 'X')) {
        const int base = candidate[1] == 'b' || candidate[1] == 'B'
                             ? 2
                             : (candidate[1] == 'o' || candidate[1] == 'O' ? 8 : 16);
        boost::multiprecision::cpp_int integer = 0;
        for (std::size_t index = 2; index < candidate.size(); ++index) {
            const char character = candidate[index];
            const int digit = character >= '0' && character <= '9'
                                  ? character - '0'
                                  : (character >= 'a' && character <= 'f'
                                         ? character - 'a' + 10
                                         : (character >= 'A' && character <= 'F'
                                                ? character - 'A' + 10
                                                : -1));
            if (digit < 0 || digit >= base) {
                return std::nullopt;
            }
            integer *= base;
            integer += digit;
        }
        const double value = integer.convert_to<double>();
        return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
    }

    std::string_view decimal = candidate;
    if (decimal.front() == '+') {
        decimal.remove_prefix(1);
        if (decimal.empty()) {
            return std::nullopt;
        }
    }
    double value = 0;
    const auto [end, error] = std::from_chars(
        decimal.data(),
        decimal.data() + decimal.size(),
        value,
        std::chars_format::general);
    if (error != std::errc{} || end != decimal.data() + decimal.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string number_to_string(double value) {
    if (value == 0) {
        return "0";
    }
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    if (error != std::errc{}) {
        return {};
    }

    const std::string shortest(buffer.data(), end);
    const auto exponent_marker = shortest.find('e');
    if (exponent_marker == std::string::npos) {
        return shortest;
    }

    std::size_t exponent_offset = exponent_marker + 1;
    bool exponent_negative = false;
    if (shortest[exponent_offset] == '+' || shortest[exponent_offset] == '-') {
        exponent_negative = shortest[exponent_offset] == '-';
        ++exponent_offset;
    }
    int exponent = 0;
    for (; exponent_offset < shortest.size(); ++exponent_offset) {
        exponent = exponent * 10 + (shortest[exponent_offset] - '0');
    }
    if (exponent_negative) {
        exponent = -exponent;
    }

    const bool negative = shortest.front() == '-';
    const std::size_t mantissa_start = negative ? 1 : 0;
    const auto decimal_point = shortest.find('.', mantissa_start);
    const std::size_t digits_before_decimal =
        (decimal_point == std::string::npos ? exponent_marker : decimal_point) - mantissa_start;
    std::string digits = shortest.substr(mantissa_start, exponent_marker - mantissa_start);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());

    // ECMAScript Number::toString uses fixed notation for this exponent range,
    // while std::to_chars(general) is permitted to choose scientific notation.
    if (exponent >= -6 && exponent < 21) {
        const auto decimal_position = static_cast<int>(digits_before_decimal) + exponent;
        std::string result = negative ? "-" : "";
        if (decimal_position <= 0) {
            result += "0.";
            result.append(static_cast<std::size_t>(-decimal_position), '0');
            result += digits;
        } else if (decimal_position >= static_cast<int>(digits.size())) {
            result += digits;
            result.append(
                static_cast<std::size_t>(decimal_position) - digits.size(),
                '0');
        } else {
            result.append(digits, 0, static_cast<std::size_t>(decimal_position));
            result.push_back('.');
            result.append(digits, static_cast<std::size_t>(decimal_position), std::string::npos);
        }
        return result;
    }

    return shortest.substr(0, exponent_marker) + "e" +
           (exponent >= 0 ? "+" : "") + std::to_string(exponent);
}

[[nodiscard]] util::JsonValue coerce_primitive(const util::JsonValue& value, JsonType type) {
    switch (type) {
    case JsonType::Number:
        if (value.holds<util::JsonValue::null_t>()) {
            return util::JsonValue{0};
        }
        if (const auto* text = value.get_if<std::string>()) {
            if (const auto parsed = parse_finite_number(*text)) {
                return util::JsonValue{*parsed};
            }
        }
        if (const auto* flag = value.get_if<bool>()) {
            return util::JsonValue{*flag ? 1 : 0};
        }
        break;
    case JsonType::Integer:
        if (value.holds<util::JsonValue::null_t>()) {
            return util::JsonValue{0};
        }
        if (const auto* text = value.get_if<std::string>()) {
            if (const auto parsed = parse_finite_number(*text); parsed && std::trunc(*parsed) == *parsed) {
                return util::JsonValue{*parsed};
            }
        }
        if (const auto* flag = value.get_if<bool>()) {
            return util::JsonValue{*flag ? 1 : 0};
        }
        break;
    case JsonType::Boolean:
        if (value.holds<util::JsonValue::null_t>()) {
            return util::JsonValue{false};
        }
        if (const auto* text = value.get_if<std::string>()) {
            if (*text == "true") {
                return util::JsonValue{true};
            }
            if (*text == "false") {
                return util::JsonValue{false};
            }
        }
        if (const auto* number = value.get_if<double>()) {
            if (*number == 1) {
                return util::JsonValue{true};
            }
            if (*number == 0) {
                return util::JsonValue{false};
            }
        }
        break;
    case JsonType::String:
        if (value.holds<util::JsonValue::null_t>()) {
            return util::JsonValue{""};
        }
        if (const auto* number = value.get_if<double>()) {
            return util::JsonValue{number_to_string(*number)};
        }
        if (const auto* flag = value.get_if<bool>()) {
            return util::JsonValue{*flag ? "true" : "false"};
        }
        break;
    case JsonType::Null:
        if (const auto* text = value.get_if<std::string>(); text != nullptr && text->empty()) {
            return util::JsonValue{nullptr};
        }
        if (const auto* number = value.get_if<double>(); number != nullptr && *number == 0) {
            return util::JsonValue{nullptr};
        }
        if (const auto* flag = value.get_if<bool>(); flag != nullptr && !*flag) {
            return util::JsonValue{nullptr};
        }
        break;
    case JsonType::Array:
    case JsonType::Object:
        break;
    }
    return value;
}

[[nodiscard]] bool includes_type(const CompiledSchema& schema, JsonType type) {
    return std::find(schema.types.begin(), schema.types.end(), type) != schema.types.end();
}

void coerce_value(util::JsonValue& value, const CompiledSchema& schema) {
    const bool already_matches_union = schema.types.size() > 1 &&
        std::any_of(schema.types.begin(), schema.types.end(), [&](JsonType type) {
            return matches_type(value, type);
        });
    if (!schema.types.empty() && !already_matches_union) {
        for (const auto type : schema.types) {
            auto candidate = coerce_primitive(value, type);
            if (!json_equal(candidate, value)) {
                value = std::move(candidate);
                break;
            }
        }
    }

    auto* object = value.get_if<util::JsonValue::object_t>();
    if (object == nullptr || !includes_type(schema, JsonType::Object)) {
        return;
    }
    for (auto& [name, property_schema] : schema.properties) {
        if (auto it = object->find(name); it != object->end()) {
            coerce_value(it->second, property_schema);
        }
    }
    if (schema.additional_properties == AdditionalPropertiesMode::Schema) {
        for (auto& [name, property_value] : *object) {
            if (!schema.properties.contains(name)) {
                coerce_value(property_value, *schema.additional_properties_schema);
            }
        }
    }
}

void add_failure(
    std::vector<ValidationFailure>& failures,
    std::string location,
    std::string reason) {
    if (failures.size() < kMaxValidationFailures) {
        failures.push_back({std::move(location), std::move(reason)});
    }
}

void validate_value(
    const util::JsonValue& value,
    const CompiledSchema& schema,
    std::string location,
    std::vector<ValidationFailure>& failures) {
    if (failures.size() >= kMaxValidationFailures) {
        return;
    }
    if (schema.reject_all) {
        add_failure(failures, std::move(location), "value is rejected by the boolean schema");
        return;
    }

    if (!schema.types.empty() &&
        !std::any_of(schema.types.begin(), schema.types.end(), [&](JsonType type) {
            return matches_type(value, type);
        })) {
        add_failure(failures, location, "value does not match an allowed JSON type");
    }

    if (schema.enum_values &&
        std::none_of(schema.enum_values->begin(), schema.enum_values->end(), [&](const auto& candidate) {
            return json_equal(value, candidate);
        })) {
        add_failure(failures, location, "value is not one of the allowed enum values");
    }
    if (schema.constant && !json_equal(value, *schema.constant)) {
        add_failure(failures, location, "value does not equal the required constant");
    }

    const auto* object = value.get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return;
    }
    for (const auto& required : schema.required) {
        if (!object->contains(required)) {
            add_failure(
                failures,
                instance_child_path(location, required),
                "required member is missing");
        }
    }
    for (const auto& [name, property_schema] : schema.properties) {
        if (const auto it = object->find(name); it != object->end()) {
            validate_value(
                it->second,
                property_schema,
                instance_child_path(location, name),
                failures);
        }
    }
    for (const auto& [name, property_value] : *object) {
        if (schema.properties.contains(name)) {
            continue;
        }
        if (schema.additional_properties == AdditionalPropertiesMode::Deny) {
            add_failure(
                failures,
                instance_child_path(location, name),
                "additional member is not allowed");
        } else if (schema.additional_properties == AdditionalPropertiesMode::Schema) {
            validate_value(
                property_value,
                *schema.additional_properties_schema,
                instance_child_path(location, name),
                failures);
        }
    }
}

[[nodiscard]] std::string validation_diagnostic(
    const std::string& tool_name,
    const std::vector<ValidationFailure>& failures) {
    std::string diagnostic = "Tool Argument Contract validation failed for tool \"" +
                             bounded_tool_argument_component(tool_name, 256) + "\":";
    for (const auto& failure : failures) {
        diagnostic += "\n  - " +
                      bounded_tool_argument_component(failure.location, 256) + ": " +
                      bounded_tool_argument_component(failure.reason, 192);
    }
    if (failures.size() == kMaxValidationFailures) {
        diagnostic += "\n  - validation failure reporting limit reached";
    }
    return bounded_tool_argument_diagnostic(std::move(diagnostic));
}

[[nodiscard]] util::Expected<util::JsonValue> parse_and_clone_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call) {
    if (!call.arguments_valid) {
        return std::unexpected(preparation_error(
            tool.name,
            "arguments are malformed JSON" +
                (call.argument_error && !call.argument_error->empty()
                     ? std::string(" (parser detail: ") + *call.argument_error + ")"
                     : std::string{}),
            util::ErrorCode::JsonParse));
    }
    if (!call.raw_arguments.empty()) {
        auto parsed = util::read_json<util::JsonValue>(call.raw_arguments);
        if (!parsed) {
            return std::unexpected(preparation_error(
                tool.name,
                "arguments are malformed JSON (parser detail: " + parsed.error().detail + ")",
                util::ErrorCode::JsonParse));
        }
        return *parsed;
    }
    if (call.arguments) {
        return *call.arguments;
    }
    auto parsed = util::read_json<util::JsonValue>("{}");
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

} // namespace

util::Expected<util::JsonValue> prepare_tool_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call) {
    auto arguments = parse_and_clone_arguments(tool, call);
    if (!arguments) {
        return std::unexpected(arguments.error());
    }

    CompilationState compilation_state;
    auto schema = compile_schema(tool.parameters, "schema", 0, compilation_state);
    if (!schema) {
        return std::unexpected(preparation_error(tool.name, schema.error().detail));
    }

    coerce_value(*arguments, *schema);
    std::vector<ValidationFailure> failures;
    validate_value(*arguments, *schema, "root", failures);
    if (!failures.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "tool arguments do not satisfy their contract",
            validation_diagnostic(tool.name, failures)));
    }
    return arguments;
}

} // namespace cch::agent
