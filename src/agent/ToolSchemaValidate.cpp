#include "ToolArgumentDetail.hpp"

#include "ToolArgumentPreparation.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include "support/BoundedText.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::agent {
namespace {

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

[[nodiscard]] bool matches_type(const support::JsonValue& value, JsonType type) {
    switch (type) {
    case JsonType::Null:
        return value.holds<support::JsonValue::null_t>();
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
        return value.holds<support::JsonValue::array_t>();
    case JsonType::Object:
        return value.holds<support::JsonValue::object_t>();
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
    std::erase(digits, '.');

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

[[nodiscard]] bool includes_type(const CompiledSchema& schema, JsonType type) {
    return std::find(schema.types.begin(), schema.types.end(), type) != schema.types.end();
}

[[nodiscard]] bool value_satisfies(
    const support::JsonValue& value,
    const CompiledSchema& schema);

void coerce_with_union(
    support::JsonValue& value,
    const std::vector<CompiledSchema>& alternatives) {
    for (const auto& alternative : alternatives) {
        auto candidate = value;
        coerce_value(candidate, alternative);
        if (value_satisfies(candidate, alternative)) {
            value = std::move(candidate);
            return;
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

[[nodiscard]] bool is_multiple_of(double value, double divisor) {
    if (std::trunc(value) == value && std::fmod(1.0 / divisor, 1.0) == 0.0) {
        return true;
    }
    const double remainder = std::fmod(value, divisor);
    return std::min(std::abs(remainder), std::abs(remainder - divisor)) < 1e-10;
}

[[nodiscard]] bool value_satisfies(
    const support::JsonValue& value,
    const CompiledSchema& schema) {
    std::vector<ValidationFailure> failures;
    validate_value(value, schema, "root", failures);
    return failures.empty();
}

[[nodiscard]] support::JsonValue coerce_primitive(const support::JsonValue& value, JsonType type) {
    switch (type) {
    case JsonType::Number:
        if (value.holds<support::JsonValue::null_t>()) {
            return support::JsonValue{0};
        }
        if (const auto* text = value.get_if<std::string>()) {
            if (const auto parsed = parse_finite_number(*text)) {
                return support::JsonValue{*parsed};
            }
        }
        if (const auto* flag = value.get_if<bool>()) {
            return support::JsonValue{*flag ? 1 : 0};
        }
        break;
    case JsonType::Integer:
        if (value.holds<support::JsonValue::null_t>()) {
            return support::JsonValue{0};
        }
        if (const auto* text = value.get_if<std::string>()) {
            if (const auto parsed = parse_finite_number(*text); parsed && std::trunc(*parsed) == *parsed) {
                return support::JsonValue{*parsed};
            }
        }
        if (const auto* flag = value.get_if<bool>()) {
            return support::JsonValue{*flag ? 1 : 0};
        }
        break;
    case JsonType::Boolean:
        if (value.holds<support::JsonValue::null_t>()) {
            return support::JsonValue{false};
        }
        if (const auto* text = value.get_if<std::string>()) {
            if (*text == "true") {
                return support::JsonValue{true};
            }
            if (*text == "false") {
                return support::JsonValue{false};
            }
        }
        if (const auto* number = value.get_if<double>()) {
            if (*number == 1) {
                return support::JsonValue{true};
            }
            if (*number == 0) {
                return support::JsonValue{false};
            }
        }
        break;
    case JsonType::String:
        if (value.holds<support::JsonValue::null_t>()) {
            return support::JsonValue{""};
        }
        if (const auto* number = value.get_if<double>()) {
            return support::JsonValue{number_to_string(*number)};
        }
        if (const auto* flag = value.get_if<bool>()) {
            return support::JsonValue{*flag ? "true" : "false"};
        }
        break;
    case JsonType::Null:
        if (const auto* text = value.get_if<std::string>(); text != nullptr && text->empty()) {
            return support::JsonValue{nullptr};
        }
        if (const auto* number = value.get_if<double>(); number != nullptr && *number == 0) {
            return support::JsonValue{nullptr};
        }
        if (const auto* flag = value.get_if<bool>(); flag != nullptr && !*flag) {
            return support::JsonValue{nullptr};
        }
        break;
    case JsonType::Array:
    case JsonType::Object:
        break;
    }
    return value;
}

} // namespace


void validate_value(
    const support::JsonValue& value,
    const CompiledSchema& schema,
    std::string location,
    std::vector<ValidationFailure>& failures) {
    if (failures.size() >= kMaxValidationFailures) return;
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

    if (const auto* number = value.get_if<double>()) {
        if (schema.minimum && *number < *schema.minimum) {
            add_failure(failures, location, "number is below minimum");
        }
        if (schema.maximum && *number > *schema.maximum) {
            add_failure(failures, location, "number is above maximum");
        }
        if (schema.exclusive_minimum && *number <= *schema.exclusive_minimum) {
            add_failure(failures, location, "number is not above exclusiveMinimum");
        }
        if (schema.exclusive_maximum && *number >= *schema.exclusive_maximum) {
            add_failure(failures, location, "number is not below exclusiveMaximum");
        }
        if (schema.multiple_of && !is_multiple_of(*number, *schema.multiple_of)) {
            add_failure(failures, location, "number is not a multipleOf the required value");
        }
    }

    if (const auto* text = value.get_if<std::string>()) {
        if (schema.min_length && !typebox_min_length_matches(*text, *schema.min_length)) {
            add_failure(failures, location, "string is shorter than minLength");
        }
        if (schema.max_length && !typebox_max_length_matches(*text, *schema.max_length)) {
            add_failure(failures, location, "string is longer than maxLength");
        }
        if (schema.pattern) {
            const auto wide_text = utf8_to_wide(*text);
            if (!std::regex_search(wide_text.begin(), wide_text.end(), *schema.pattern)) {
                add_failure(failures, location, "string does not match the required pattern");
            }
        }
        if (schema.format && !matches_format(*schema.format, *text)) {
            add_failure(failures, location, "string does not match the required format");
        }
    }

    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        if (schema.min_properties && object->size() < *schema.min_properties) {
            add_failure(failures, location, "object has fewer members than minProperties");
        }
        if (schema.max_properties && object->size() > *schema.max_properties) {
            add_failure(failures, location, "object has more members than maxProperties");
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
            if (schema.properties.contains(name)) continue;
            if (schema.additional_properties == AdditionalValueMode::Deny) {
                add_failure(
                    failures,
                    instance_child_path(location, name),
                    "additional member is not allowed");
            } else if (schema.additional_properties == AdditionalValueMode::Schema) {
                validate_value(
                    property_value,
                    *schema.additional_properties_schema,
                    instance_child_path(location, name),
                    failures);
            }
        }
    }

    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        if (schema.min_items && array->size() < *schema.min_items) {
            add_failure(failures, location, "array has fewer elements than minItems");
        }
        if (schema.max_items && array->size() > *schema.max_items) {
            add_failure(failures, location, "array has more elements than maxItems");
        }
        if (schema.unique_items) {
            bool duplicate = false;
            for (std::size_t left = 0; left < array->size() && !duplicate; ++left) {
                for (std::size_t right = left + 1; right < array->size(); ++right) {
                    if (json_equal((*array)[left], (*array)[right])) {
                        duplicate = true;
                        break;
                    }
                }
            }
            if (duplicate) add_failure(failures, location, "array elements are not unique");
        }
        if (schema.items == ItemsMode::Schema) {
            for (std::size_t index = 0; index < array->size(); ++index) {
                validate_value(
                    (*array)[index],
                    *schema.item_schema,
                    instance_child_path(location, std::to_string(index)),
                    failures);
            }
        } else if (schema.items == ItemsMode::Tuple) {
            const auto count = std::min(array->size(), schema.tuple_items.size());
            for (std::size_t index = 0; index < count; ++index) {
                validate_value(
                    (*array)[index],
                    schema.tuple_items[index],
                    instance_child_path(location, std::to_string(index)),
                    failures);
            }
            for (std::size_t index = schema.tuple_items.size(); index < array->size(); ++index) {
                if (schema.additional_items == AdditionalValueMode::Deny) {
                    add_failure(
                        failures,
                        instance_child_path(location, std::to_string(index)),
                        "additional tuple element is not allowed");
                } else if (schema.additional_items == AdditionalValueMode::Schema) {
                    validate_value(
                        (*array)[index],
                        *schema.additional_items_schema,
                        instance_child_path(location, std::to_string(index)),
                        failures);
                }
            }
        }
    }

    for (const auto& conjunct : schema.all_of) {
        validate_value(value, conjunct, location, failures);
    }
    if (schema.has_any_of) {
        const bool matched = std::any_of(
            schema.any_of.begin(), schema.any_of.end(), [&](const CompiledSchema& alternative) {
                return value_satisfies(value, alternative);
            });
        if (!matched) add_failure(failures, location, "value does not satisfy anyOf");
    }
    if (schema.has_one_of) {
        const auto matches = std::count_if(
            schema.one_of.begin(), schema.one_of.end(), [&](const CompiledSchema& alternative) {
                return value_satisfies(value, alternative);
            });
        if (matches != 1) add_failure(failures, location, "value does not satisfy exactly one oneOf branch");
    }
}


void coerce_value(support::JsonValue& value, const CompiledSchema& schema) {
    for (const auto& conjunct : schema.all_of) {
        coerce_value(value, conjunct);
    }
    if (!schema.any_of.empty()) {
        coerce_with_union(value, schema.any_of);
    }
    if (!schema.one_of.empty()) {
        coerce_with_union(value, schema.one_of);
    }

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

    if (auto* object = value.get_if<support::JsonValue::object_t>();
        object != nullptr && includes_type(schema, JsonType::Object)) {
        for (auto& [name, property_schema] : schema.properties) {
            if (auto it = object->find(name); it != object->end()) {
                coerce_value(it->second, property_schema);
            }
        }
        if (schema.additional_properties == AdditionalValueMode::Schema) {
            for (auto& [name, property_value] : *object) {
                if (!schema.properties.contains(name)) {
                    coerce_value(property_value, *schema.additional_properties_schema);
                }
            }
        }
    }

    if (auto* array = value.get_if<support::JsonValue::array_t>();
        array != nullptr && includes_type(schema, JsonType::Array)) {
        if (schema.items == ItemsMode::Schema) {
            for (auto& item : *array) {
                coerce_value(item, *schema.item_schema);
            }
        } else if (schema.items == ItemsMode::Tuple) {
            const auto count = std::min(array->size(), schema.tuple_items.size());
            for (std::size_t index = 0; index < count; ++index) {
                coerce_value((*array)[index], schema.tuple_items[index]);
            }
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

} // namespace cch::agent
