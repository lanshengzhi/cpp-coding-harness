#include "ToolArgumentPreparation.hpp"

#include "support/Json.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
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

enum class AdditionalValueMode {
    Allow,
    Deny,
    Schema,
};

enum class ItemsMode {
    None,
    Schema,
    Tuple,
};

enum class FormatKind {
    DateTime,
    Date,
    Duration,
    Email,
    Hostname,
    IdnEmail,
    IdnHostname,
    Ipv4,
    Ipv6,
    IriReference,
    Iri,
    JsonPointerUriFragment,
    JsonPointer,
    Regex,
    RelativeJsonPointer,
    Time,
    UriReference,
    UriTemplate,
    Uri,
    Url,
    Uuid,
};

struct CompiledSchema {
    bool reject_all{false};
    std::vector<JsonType> types;
    std::optional<support::JsonValue::array_t> enum_values;
    std::optional<support::JsonValue> constant;

    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> exclusive_minimum;
    std::optional<double> exclusive_maximum;
    std::optional<double> multiple_of;

    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
    std::optional<std::wregex> pattern;
    std::optional<FormatKind> format;

    std::optional<std::size_t> min_properties;
    std::optional<std::size_t> max_properties;
    std::map<std::string, CompiledSchema> properties;
    std::vector<std::string> required;
    AdditionalValueMode additional_properties{AdditionalValueMode::Allow};
    std::unique_ptr<CompiledSchema> additional_properties_schema;

    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    bool unique_items{false};
    ItemsMode items{ItemsMode::None};
    std::unique_ptr<CompiledSchema> item_schema;
    std::vector<CompiledSchema> tuple_items;
    AdditionalValueMode additional_items{AdditionalValueMode::Allow};
    std::unique_ptr<CompiledSchema> additional_items_schema;

    bool has_any_of{false};
    bool has_one_of{false};
    std::vector<CompiledSchema> all_of;
    std::vector<CompiledSchema> any_of;
    std::vector<CompiledSchema> one_of;
};

struct CompilationState {
    std::size_t nodes{0};
};

struct CompilationContext {
    bool require_known_formats{false};
};

struct ValidationFailure {
    std::string location;
    std::string reason;
};

[[nodiscard]] support::Error preparation_error(
    const std::string& tool_name,
    std::string reason,
    support::ErrorCode code = support::ErrorCode::Validation) {
    return support::make_error(
        code,
        "tool argument preparation failed",
        bounded_tool_argument_diagnostic(
            "Tool Argument Contract preparation failed at root for tool \"" +
            bounded_tool_argument_component(tool_name, 256) + "\": " +
            bounded_tool_argument_component(std::move(reason), 3500)));
}

[[nodiscard]] support::Error malformed_arguments_error(
    const std::string& tool_name,
    std::string parser_detail) {
    std::string diagnostic =
        "Tool Argument Contract preparation failed at root for tool \"" +
        bounded_tool_argument_component(tool_name, 256) +
        "\": arguments are malformed JSON";
    if (!parser_detail.empty()) {
        diagnostic += " (parser detail: " + std::move(parser_detail) + ")";
    }
    return support::make_error(
        support::ErrorCode::JsonParse,
        "tool argument preparation failed",
        ai::bounded_redacted_text(
            std::move(diagnostic),
            4096,
            " [diagnostic truncated]"));
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

[[nodiscard]] bool json_equal(const support::JsonValue& left, const support::JsonValue& right) {
    if (left.data.index() != right.data.index()) {
        return false;
    }
    if (left.holds<support::JsonValue::null_t>()) {
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
    if (const auto* array = left.get_if<support::JsonValue::array_t>()) {
        const auto& other = right.get<support::JsonValue::array_t>();
        return array->size() == other.size() &&
               std::equal(array->begin(), array->end(), other.begin(), json_equal);
    }

    const auto& object = left.get<support::JsonValue::object_t>();
    const auto& other = right.get<support::JsonValue::object_t>();
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

[[nodiscard]] std::vector<std::uint32_t> decode_utf8(std::string_view value) {
    std::vector<std::uint32_t> points;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::uint32_t point = first;
        std::size_t length = 1;
        if ((first & 0xe0) == 0xc0 && index + 1 < value.size()) {
            point = first & 0x1f;
            length = 2;
        } else if ((first & 0xf0) == 0xe0 && index + 2 < value.size()) {
            point = first & 0x0f;
            length = 3;
        } else if ((first & 0xf8) == 0xf0 && index + 3 < value.size()) {
            point = first & 0x07;
            length = 4;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            point = (point << 6) | (continuation & 0x3f);
        }
        if (!valid) {
            point = first;
            length = 1;
        }
        points.push_back(point);
        index += length;
    }
    return points;
}

[[nodiscard]] std::wstring utf8_to_wide(std::string_view value) {
    std::wstring result;
    for (const auto point : decode_utf8(value)) {
        if constexpr (sizeof(wchar_t) >= 4) {
            result.push_back(static_cast<wchar_t>(point));
        } else {
            // A single private-use token preserves one-code-point regex width.
            // Schemas with non-BMP literals fail compilation below on such hosts.
            result.push_back(static_cast<wchar_t>(point <= 0xffff ? point : 0xe000));
        }
    }
    return result;
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
    // The executable profile is deliberately smaller than transported JSON
    // Schema. Recognized constructs outside it fail closed instead of becoming
    // accidental annotations.
    constexpr std::array<std::string_view, 19> unsupported{
        "$dynamicRef", "$recursiveRef", "$ref", "contains", "dependentRequired",
        "dependentSchemas", "dependencies", "else", "if", "maxContains",
        "minContains", "not", "patternProperties", "prefixItems", "propertyNames",
        "then", "unevaluatedItems", "unevaluatedProperties", "$recursiveAnchor",
    };
    return std::find(unsupported.begin(), unsupported.end(), keyword) != unsupported.end();
}

[[nodiscard]] support::Expected<std::vector<JsonType>> compile_types(
    const support::JsonValue& value,
    const std::string& schema_path) {
    std::vector<JsonType> result;
    auto add_type = [&](const support::JsonValue& candidate) -> support::ExpectedVoid {
        const auto* type_name = candidate.get_if<std::string>();
        if (type_name == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                schema_path + " must contain only JSON type names"));
        }
        const auto type = json_type_from_name(*type_name);
        if (!type) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "invalid Tool Argument Contract",
                schema_path + " contains unsupported JSON type \"" + *type_name + "\""));
        }
        if (std::find(result.begin(), result.end(), *type) != result.end()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
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
    const auto* values = value.get_if<support::JsonValue::array_t>();
    if (values == nullptr || values->empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

[[nodiscard]] support::Error schema_compile_error(
    std::string message,
    std::string detail) {
    return support::make_error(
        support::ErrorCode::Validation,
        std::move(message),
        std::move(detail));
}

[[nodiscard]] bool is_supported_dialect(std::string_view dialect) {
    // The recorded profile uses draft-07 tuple-valued items. Later dialects
    // assign different meanings to the same members (notably `items`), so they
    // cannot be accepted without dialect-specific compilation.
    return dialect == "http://json-schema.org/draft-07/schema#" ||
           dialect == "https://json-schema.org/draft-07/schema#";
}

[[nodiscard]] bool is_supported_vocabulary(std::string_view vocabulary) {
    constexpr std::array<std::string_view, 15> supported{
        "https://json-schema.org/draft/2019-09/vocab/core",
        "https://json-schema.org/draft/2019-09/vocab/applicator",
        "https://json-schema.org/draft/2019-09/vocab/validation",
        "https://json-schema.org/draft/2019-09/vocab/meta-data",
        "https://json-schema.org/draft/2019-09/vocab/format",
        "https://json-schema.org/draft/2019-09/vocab/content",
        "https://json-schema.org/draft/2020-12/vocab/core",
        "https://json-schema.org/draft/2020-12/vocab/applicator",
        "https://json-schema.org/draft/2020-12/vocab/validation",
        "https://json-schema.org/draft/2020-12/vocab/meta-data",
        "https://json-schema.org/draft/2020-12/vocab/format-annotation",
        "https://json-schema.org/draft/2020-12/vocab/format-assertion",
        "https://json-schema.org/draft/2020-12/vocab/content",
        "http://json-schema.org/draft-07/schema#",
        "http://json-schema.org/draft-06/schema#",
    };
    return std::find(supported.begin(), supported.end(), vocabulary) != supported.end();
}

[[nodiscard]] bool is_format_assertion_vocabulary(std::string_view vocabulary) {
    return vocabulary == "https://json-schema.org/draft/2019-09/vocab/format" ||
           vocabulary == "https://json-schema.org/draft/2020-12/vocab/format-assertion";
}

[[nodiscard]] std::optional<FormatKind> format_kind(std::string_view format) {
    if (format == "date-time") return FormatKind::DateTime;
    if (format == "date") return FormatKind::Date;
    if (format == "duration") return FormatKind::Duration;
    if (format == "email") return FormatKind::Email;
    if (format == "hostname") return FormatKind::Hostname;
    if (format == "idn-email") return FormatKind::IdnEmail;
    if (format == "idn-hostname") return FormatKind::IdnHostname;
    if (format == "ipv4") return FormatKind::Ipv4;
    if (format == "ipv6") return FormatKind::Ipv6;
    if (format == "iri-reference") return FormatKind::IriReference;
    if (format == "iri") return FormatKind::Iri;
    if (format == "json-pointer-uri-fragment") return FormatKind::JsonPointerUriFragment;
    if (format == "json-pointer") return FormatKind::JsonPointer;
    if (format == "regex") return FormatKind::Regex;
    if (format == "relative-json-pointer") return FormatKind::RelativeJsonPointer;
    if (format == "time") return FormatKind::Time;
    if (format == "uri-reference") return FormatKind::UriReference;
    if (format == "uri-template") return FormatKind::UriTemplate;
    if (format == "uri") return FormatKind::Uri;
    if (format == "url") return FormatKind::Url;
    if (format == "uuid") return FormatKind::Uuid;
    return std::nullopt;
}

[[nodiscard]] support::Expected<std::optional<double>> compile_number_keyword(
    const support::JsonValue::object_t& object,
    std::string_view keyword,
    const std::string& schema_path,
    bool positive = false) {
    const auto it = object.find(std::string(keyword));
    if (it == object.end()) {
        return std::optional<double>{};
    }
    const auto* number = it->second.get_if<double>();
    if (number == nullptr || !std::isfinite(*number) || (positive && *number <= 0)) {
        return std::unexpected(schema_compile_error(
            "invalid Tool Argument Contract",
            child_path(schema_path, keyword) +
                (positive ? " must be a finite number greater than zero"
                          : " must be a finite number")));
    }
    return std::optional<double>{*number};
}

[[nodiscard]] support::Expected<std::optional<std::size_t>> compile_size_keyword(
    const support::JsonValue::object_t& object,
    std::string_view keyword,
    const std::string& schema_path) {
    const auto it = object.find(std::string(keyword));
    if (it == object.end()) {
        return std::optional<std::size_t>{};
    }
    const auto* number = it->second.get_if<double>();
    if (number == nullptr || !std::isfinite(*number) || *number < 0 ||
        std::trunc(*number) != *number ||
        *number > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(schema_compile_error(
            "invalid Tool Argument Contract",
            child_path(schema_path, keyword) + " must be a non-negative integer"));
    }
    return std::optional<std::size_t>{static_cast<std::size_t>(*number)};
}

[[nodiscard]] support::Expected<CompiledSchema> compile_schema(
    const support::JsonValue& schema,
    std::string schema_path,
    std::size_t depth,
    CompilationState& state,
    CompilationContext context) {
    if (depth > kMaxSchemaDepth || ++state.nodes > kMaxSchemaNodes) {
        return std::unexpected(schema_compile_error(
            "invalid Tool Argument Contract",
            schema_path + " exceeds the executable schema complexity limit"));
    }

    if (const auto* boolean_schema = schema.get_if<bool>()) {
        CompiledSchema compiled;
        compiled.reject_all = !*boolean_schema;
        return compiled;
    }
    const auto* object = schema.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(schema_compile_error(
            "invalid Tool Argument Contract",
            schema_path + " must be an object or boolean schema"));
    }

    if (const auto it = object->find("$schema"); it != object->end()) {
        const auto* dialect = it->second.get_if<std::string>();
        if (dialect == nullptr || !is_supported_dialect(*dialect)) {
            return std::unexpected(schema_compile_error(
                "unsupported Tool Argument Contract",
                child_path(schema_path, "$schema") + " declares an unsupported schema dialect"));
        }
    }
    if (const auto it = object->find("$vocabulary"); it != object->end()) {
        const auto* vocabularies = it->second.get_if<support::JsonValue::object_t>();
        if (vocabularies == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "$vocabulary") + " must be an object"));
        }
        for (const auto& [name, requirement] : *vocabularies) {
            const auto* required = requirement.get_if<bool>();
            if (required == nullptr) {
                return std::unexpected(schema_compile_error(
                    "invalid Tool Argument Contract",
                    child_path(child_path(schema_path, "$vocabulary"), name) +
                        " must be boolean"));
            }
            if (*required && !is_supported_vocabulary(name)) {
                return std::unexpected(schema_compile_error(
                    "unsupported Tool Argument Contract",
                    child_path(child_path(schema_path, "$vocabulary"), name) +
                        " is a required unsupported vocabulary"));
            }
            context.require_known_formats =
                context.require_known_formats || (*required && is_format_assertion_vocabulary(name));
        }
    }

    for (const auto& [keyword, _] : *object) {
        if (is_known_unsupported_keyword(keyword)) {
            return std::unexpected(schema_compile_error(
                "unsupported Tool Argument Contract",
                child_path(schema_path, keyword) +
                    " is a recognized executable construct outside the recorded profile"));
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
        const auto* values = it->second.get_if<support::JsonValue::array_t>();
        if (values == nullptr || values->empty()) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "enum") + " must be a non-empty array"));
        }
        for (std::size_t left = 0; left < values->size(); ++left) {
            for (std::size_t right = left + 1; right < values->size(); ++right) {
                if (json_equal((*values)[left], (*values)[right])) {
                    return std::unexpected(schema_compile_error(
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

    auto minimum = compile_number_keyword(*object, "minimum", schema_path);
    auto maximum = compile_number_keyword(*object, "maximum", schema_path);
    auto exclusive_minimum = compile_number_keyword(*object, "exclusiveMinimum", schema_path);
    auto exclusive_maximum = compile_number_keyword(*object, "exclusiveMaximum", schema_path);
    auto multiple_of = compile_number_keyword(*object, "multipleOf", schema_path, true);
    if (!minimum) return std::unexpected(minimum.error());
    if (!maximum) return std::unexpected(maximum.error());
    if (!exclusive_minimum) return std::unexpected(exclusive_minimum.error());
    if (!exclusive_maximum) return std::unexpected(exclusive_maximum.error());
    if (!multiple_of) return std::unexpected(multiple_of.error());
    compiled.minimum = *minimum;
    compiled.maximum = *maximum;
    compiled.exclusive_minimum = *exclusive_minimum;
    compiled.exclusive_maximum = *exclusive_maximum;
    compiled.multiple_of = *multiple_of;

    auto min_length = compile_size_keyword(*object, "minLength", schema_path);
    auto max_length = compile_size_keyword(*object, "maxLength", schema_path);
    auto min_properties = compile_size_keyword(*object, "minProperties", schema_path);
    auto max_properties = compile_size_keyword(*object, "maxProperties", schema_path);
    auto min_items = compile_size_keyword(*object, "minItems", schema_path);
    auto max_items = compile_size_keyword(*object, "maxItems", schema_path);
    if (!min_length) return std::unexpected(min_length.error());
    if (!max_length) return std::unexpected(max_length.error());
    if (!min_properties) return std::unexpected(min_properties.error());
    if (!max_properties) return std::unexpected(max_properties.error());
    if (!min_items) return std::unexpected(min_items.error());
    if (!max_items) return std::unexpected(max_items.error());
    compiled.min_length = *min_length;
    compiled.max_length = *max_length;
    compiled.min_properties = *min_properties;
    compiled.max_properties = *max_properties;
    compiled.min_items = *min_items;
    compiled.max_items = *max_items;

    if (const auto it = object->find("pattern"); it != object->end()) {
        const auto* pattern = it->second.get_if<std::string>();
        if (pattern == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "pattern") + " must be a string"));
        }
        const bool unsupported_unicode_escape =
            pattern->find("\\p{") != std::string::npos ||
            pattern->find("\\P{") != std::string::npos ||
            pattern->find("\\u{") != std::string::npos;
        bool non_bmp_literal_is_unrepresentable = false;
        if constexpr (sizeof(wchar_t) < 4) {
            const auto pattern_points = decode_utf8(*pattern);
            non_bmp_literal_is_unrepresentable = std::any_of(
                pattern_points.begin(), pattern_points.end(),
                [](std::uint32_t point) { return point > 0xffff; });
        }
        if (unsupported_unicode_escape || non_bmp_literal_is_unrepresentable) {
            return std::unexpected(schema_compile_error(
                "unsupported Tool Argument Contract",
                child_path(schema_path, "pattern") +
                    " uses Unicode regular-expression syntax that cannot be enforced"));
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            compiled.pattern.emplace(utf8_to_wide(*pattern), std::regex::ECMAScript);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::regex_error&) {
            return std::unexpected(schema_compile_error(
                "unsupported Tool Argument Contract",
                child_path(schema_path, "pattern") + " cannot be enforced as a regular expression"));
        }
#endif
    }
    if (const auto it = object->find("format"); it != object->end()) {
        const auto* format = it->second.get_if<std::string>();
        if (format == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "format") + " must be a string"));
        }
        compiled.format = format_kind(*format);
        if (!compiled.format && context.require_known_formats) {
            return std::unexpected(schema_compile_error(
                "unsupported Tool Argument Contract",
                child_path(schema_path, "format") +
                    " names an unsupported format required as an assertion"));
        }
    }

    if (const auto it = object->find("properties"); it != object->end()) {
        const auto* properties = it->second.get_if<support::JsonValue::object_t>();
        if (properties == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "properties") + " must be an object"));
        }
        for (const auto& [name, property_schema] : *properties) {
            auto nested = compile_schema(
                property_schema,
                child_path(child_path(schema_path, "properties"), name),
                depth + 1,
                state,
                context);
            if (!nested) return std::unexpected(nested.error());
            compiled.properties.emplace(name, std::move(*nested));
        }
    }
    if (const auto it = object->find("required"); it != object->end()) {
        const auto* required = it->second.get_if<support::JsonValue::array_t>();
        if (required == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "required") +
                    " must be an array of unique property names"));
        }
        std::set<std::string> seen;
        for (const auto& entry : *required) {
            const auto* name = entry.get_if<std::string>();
            if (name == nullptr || !seen.insert(*name).second) {
                return std::unexpected(schema_compile_error(
                    "invalid Tool Argument Contract",
                    child_path(schema_path, "required") +
                        " must contain unique string property names"));
            }
            compiled.required.push_back(*name);
        }
    }
    if (const auto it = object->find("additionalProperties"); it != object->end()) {
        if (const auto* allow = it->second.get_if<bool>()) {
            compiled.additional_properties = *allow ? AdditionalValueMode::Allow
                                                    : AdditionalValueMode::Deny;
        } else {
            auto nested = compile_schema(
                it->second,
                child_path(schema_path, "additionalProperties"),
                depth + 1,
                state,
                context);
            if (!nested) return std::unexpected(nested.error());
            compiled.additional_properties = AdditionalValueMode::Schema;
            compiled.additional_properties_schema =
                std::make_unique<CompiledSchema>(std::move(*nested));
        }
    }

    if (const auto it = object->find("items"); it != object->end()) {
        if (const auto* tuple = it->second.get_if<support::JsonValue::array_t>()) {
            compiled.items = ItemsMode::Tuple;
            for (std::size_t index = 0; index < tuple->size(); ++index) {
                auto nested = compile_schema(
                    (*tuple)[index],
                    child_path(child_path(schema_path, "items"), std::to_string(index)),
                    depth + 1,
                    state,
                    context);
                if (!nested) return std::unexpected(nested.error());
                compiled.tuple_items.push_back(std::move(*nested));
            }
        } else {
            auto nested = compile_schema(
                it->second,
                child_path(schema_path, "items"),
                depth + 1,
                state,
                context);
            if (!nested) return std::unexpected(nested.error());
            compiled.items = ItemsMode::Schema;
            compiled.item_schema = std::make_unique<CompiledSchema>(std::move(*nested));
        }
    }
    if (const auto it = object->find("additionalItems"); it != object->end()) {
        if (const auto* allow = it->second.get_if<bool>()) {
            compiled.additional_items = *allow ? AdditionalValueMode::Allow
                                               : AdditionalValueMode::Deny;
        } else {
            auto nested = compile_schema(
                it->second,
                child_path(schema_path, "additionalItems"),
                depth + 1,
                state,
                context);
            if (!nested) return std::unexpected(nested.error());
            compiled.additional_items = AdditionalValueMode::Schema;
            compiled.additional_items_schema =
                std::make_unique<CompiledSchema>(std::move(*nested));
        }
    }
    if (const auto it = object->find("uniqueItems"); it != object->end()) {
        const auto* unique = it->second.get_if<bool>();
        if (unique == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, "uniqueItems") + " must be boolean"));
        }
        compiled.unique_items = *unique;
    }

    auto compile_schema_list = [&](std::string_view keyword, std::vector<CompiledSchema>& output)
        -> support::ExpectedVoid {
        const auto it = object->find(std::string(keyword));
        if (it == object->end()) return {};
        const auto* schemas = it->second.get_if<support::JsonValue::array_t>();
        if (schemas == nullptr) {
            return std::unexpected(schema_compile_error(
                "invalid Tool Argument Contract",
                child_path(schema_path, keyword) + " must be an array of schemas"));
        }
        for (std::size_t index = 0; index < schemas->size(); ++index) {
            auto nested = compile_schema(
                (*schemas)[index],
                child_path(child_path(schema_path, keyword), std::to_string(index)),
                depth + 1,
                state,
                context);
            if (!nested) return std::unexpected(nested.error());
            output.push_back(std::move(*nested));
        }
        return {};
    };
    compiled.has_any_of = object->contains("anyOf");
    compiled.has_one_of = object->contains("oneOf");
    if (auto result = compile_schema_list("allOf", compiled.all_of); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = compile_schema_list("anyOf", compiled.any_of); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = compile_schema_list("oneOf", compiled.one_of); !result) {
        return std::unexpected(result.error());
    }

    return compiled;
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

[[nodiscard]] bool is_ascii_alpha(unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool is_ascii_digit(unsigned char character) {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool is_ascii_hex(unsigned char character) {
    return is_ascii_digit(character) ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

[[nodiscard]] bool has_valid_percent_encoding(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') continue;
        if (index + 2 >= value.size() ||
            !is_ascii_hex(static_cast<unsigned char>(value[index + 1])) ||
            !is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
            return false;
        }
        index += 2;
    }
    return true;
}

[[nodiscard]] bool has_forbidden_reference_character(std::string_view value, bool ascii_only) {
    for (const unsigned char character : value) {
        if (character <= 0x20 || character == 0x7f || character == '\\' ||
            (ascii_only && character > 0x7f)) {
            return true;
        }
    }
    return false;
}

// The regexes below constrain every matched group to digits, so parsing
// a matched group cannot fail and the from_chars result needs no check.
template <typename T>
[[nodiscard]] T parse_matched_number(const std::ssub_match& match) {
    const char* const first = &*match.first;
    T value{};
    std::from_chars(first, first + match.length(), value);
    return value;
}

[[nodiscard]] bool is_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

[[nodiscard]] bool is_date_format(std::string_view value) {
    static const std::regex expression(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch match;
    const std::string text(value);
    if (!std::regex_match(text, match, expression)) return false;
    const int year = parse_matched_number<int>(match[1]);
    const int month = parse_matched_number<int>(match[2]);
    const int day = parse_matched_number<int>(match[3]);
    constexpr std::array<int, 13> days{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1) return false;
    const int maximum = month == 2 && is_leap_year(year) ? 29 : days[month];
    return day <= maximum;
}

[[nodiscard]] bool is_time_format(std::string_view value) {
    static const std::regex expression(
        R"(^(\d{2}):(\d{2}):(\d{2}(?:\.\d+)?)(?:[Zz]|([+-])(\d{2}):(\d{2}))$)");
    std::smatch match;
    const std::string text(value);
    if (!std::regex_match(text, match, expression)) return false;
    const int hour = parse_matched_number<int>(match[1]);
    const int minute = parse_matched_number<int>(match[2]);
    const double second = parse_matched_number<double>(match[3]);
    const int zone_sign = match[4].str() == "-" ? -1 : 1;
    const int zone_hour = match[5].matched ? parse_matched_number<int>(match[5]) : 0;
    const int zone_minute = match[6].matched ? parse_matched_number<int>(match[6]) : 0;
    if (zone_hour > 23 || zone_minute > 59) return false;
    if (hour <= 23 && minute <= 59 && second < 60) return true;
    const int utc_minute = minute - zone_minute * zone_sign;
    const int utc_hour = hour - zone_hour * zone_sign - (utc_minute < 0 ? 1 : 0);
    return (utc_hour == 23 || utc_hour == -1) &&
           (utc_minute == 59 || utc_minute == -1) && second < 61;
}

[[nodiscard]] bool is_date_time_format(std::string_view value) {
    const auto separator = value.find_first_of("Tt");
    return separator != std::string_view::npos &&
           value.find_first_of("Tt", separator + 1) == std::string_view::npos &&
           is_date_format(value.substr(0, separator)) &&
           is_time_format(value.substr(separator + 1));
}

[[nodiscard]] bool is_ipv4_format(std::string_view value) {
    int segments = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('.', start);
        const auto segment = value.substr(
            start,
            end == std::string_view::npos ? value.size() - start : end - start);
        if (segment.empty() || segment.size() > 3 ||
            (segment.size() > 1 && segment.front() == '0')) {
            return false;
        }
        int number = 0;
        for (const unsigned char character : segment) {
            if (!is_ascii_digit(character)) return false;
            number = number * 10 + (character - '0');
        }
        if (number > 255) return false;
        ++segments;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return segments == 4;
}

[[nodiscard]] bool is_ipv6_format(std::string_view value) {
    if (value.empty()) return false;
    int groups = 0;
    bool compressed = false;
    std::size_t index = 0;
    if (value.starts_with("::")) {
        if (value.size() == 2) return true;
        compressed = true;
        index = 2;
    }
    while (index < value.size()) {
        const auto start = index;
        while (index < value.size() &&
               is_ascii_hex(static_cast<unsigned char>(value[index]))) {
            ++index;
        }
        const auto digits = index - start;
        if (digits == 0) return false;
        if (index < value.size() && value[index] == '.') {
            if (!is_ipv4_format(value.substr(start))) return false;
            groups += 2;
            index = value.size();
            break;
        }
        if (digits > 4) return false;
        ++groups;
        if (index == value.size()) break;
        if (value[index] != ':') return false;
        ++index;
        if (index < value.size() && value[index] == ':') {
            if (compressed || (index + 1 < value.size() && value[index + 1] == ':')) {
                return false;
            }
            compressed = true;
            ++index;
            if (index == value.size()) break;
        }
    }
    return compressed ? groups <= 7 : groups == 8;
}

[[nodiscard]] bool is_ascii_hostname_label(std::string_view label) {
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
        return false;
    }
    for (const unsigned char character : label) {
        if (!is_ascii_alpha(character) && !is_ascii_digit(character) && character != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_hostname_format(std::string_view value) {
    if (value.empty() || value.size() > 253 || value.back() == '.') return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('.', start);
        const auto label = value.substr(
            start,
            end == std::string_view::npos ? value.size() - start : end - start);
        if (!is_ascii_hostname_label(label)) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

[[nodiscard]] bool is_unicode_hostname_separator_at(
    std::string_view value,
    std::size_t index) {
    constexpr std::array<std::string_view, 3> separators{
        "\xE3\x80\x82", // U+3002 IDEOGRAPHIC FULL STOP
        "\xEF\xBC\x8E", // U+FF0E FULLWIDTH FULL STOP
        "\xEF\xBD\xA1", // U+FF61 HALFWIDTH IDEOGRAPHIC FULL STOP
    };
    return std::any_of(separators.begin(), separators.end(), [&](std::string_view separator) {
        return value.substr(index).starts_with(separator);
    });
}

[[nodiscard]] bool contains_unicode_hostname_separator(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (is_unicode_hostname_separator_at(value, index)) return true;
    }
    return false;
}

[[nodiscard]] std::string canonicalize_hostname_separators(std::string_view value) {
    std::string canonical;
    canonical.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        if (is_unicode_hostname_separator_at(value, index)) {
            canonical.push_back('.');
            index += 3;
        } else {
            canonical.push_back(value[index]);
            ++index;
        }
    }
    return canonical;
}

[[nodiscard]] std::size_t utf16_code_unit_count(std::string_view value) {
    std::size_t count = 0;
    for (const auto point : decode_utf8(value)) {
        count += point > 0xffff ? 2 : 1;
    }
    return count;
}

[[nodiscard]] bool is_idn_hostname_format(std::string_view value) {
    const auto canonical = canonicalize_hostname_separators(value);
    if (canonical.empty() || utf16_code_unit_count(canonical) > 253 ||
        canonical.find(' ') != std::string::npos) {
        return false;
    }
    const bool all_ascii = std::all_of(
        canonical.begin(), canonical.end(),
        [](unsigned char character) { return character < 0x80; });
    if (all_ascii) return is_hostname_format(canonical);

    std::size_t start = 0;
    while (start <= canonical.size()) {
        const auto end = canonical.find('.', start);
        const auto label = std::string_view(canonical).substr(
            start,
            end == std::string_view::npos ? canonical.size() - start : end - start);
        if (label.empty() || utf16_code_unit_count(label) > 63 ||
            label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (const unsigned char character : label) {
            if (character < 0x20 || character == 0x7f) return false;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

[[nodiscard]] std::vector<std::uint32_t> utf8_code_points(std::string_view value);

[[nodiscard]] bool is_unicode_letter_or_number(std::uint32_t point) {
    return (point >= 0x00c0 && point <= 0x02ff) ||
           (point >= 0x0370 && point <= 0x052f) ||
           (point >= 0x0531 && point <= 0x058f) ||
           (point >= 0x05d0 && point <= 0x05ea) ||
           (point >= 0x0620 && point <= 0x06ff) ||
           (point >= 0x0710 && point <= 0x08ff) ||
           (point >= 0x0900 && point <= 0x1fff) ||
           (point >= 0x2c00 && point <= 0x2dff) ||
           (point >= 0x3040 && point <= 0xd7af) ||
           (point >= 0xf900 && point <= 0xfaff) ||
           (point >= 0xff10 && point <= 0xff5a) ||
           (point >= 0x10000 && point <= 0x1efff) ||
           (point >= 0x20000 && point <= 0x3134f);
}

[[nodiscard]] bool is_email_format(std::string_view value, bool international) {
    const auto separator = value.find('@');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == value.size() || value.find('@', separator + 1) != std::string_view::npos) {
        return false;
    }
    const auto local = value.substr(0, separator);
    if (local.front() == '.' || local.back() == '.' ||
        local.find("..") != std::string_view::npos) {
        return false;
    }
    constexpr std::string_view punctuation = "!#$%&'*+/=?^_`{|}~-";
    for (const auto point : utf8_code_points(local)) {
        if (point <= 0x7f) {
            const auto character = static_cast<unsigned char>(point);
            if (is_ascii_alpha(character) || is_ascii_digit(character) || character == '.' ||
                punctuation.find(static_cast<char>(character)) != std::string_view::npos) {
                continue;
            }
            return false;
        }
        if (!international || !is_unicode_letter_or_number(point)) return false;
    }
    const auto domain = value.substr(separator + 1);
    if (international && contains_unicode_hostname_separator(domain)) {
        return false;
    }
    return international ? is_idn_hostname_format(domain)
                         : is_hostname_format(domain);
}

[[nodiscard]] bool has_valid_scheme(std::string_view value) {
    const auto colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        !is_ascii_alpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (std::size_t index = 1; index < colon; ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (!is_ascii_alpha(character) && !is_ascii_digit(character) &&
            character != '+' && character != '-' && character != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_uri_unreserved(unsigned char character) {
    return is_ascii_alpha(character) || is_ascii_digit(character) ||
           character == '-' || character == '.' || character == '_' || character == '~';
}

[[nodiscard]] bool is_uri_sub_delimiter(unsigned char character) {
    constexpr std::string_view delimiters = "!$&'()*+,;=";
    return delimiters.find(static_cast<char>(character)) != std::string_view::npos;
}

[[nodiscard]] bool is_uri_path_character(unsigned char character) {
    return is_uri_unreserved(character) || is_uri_sub_delimiter(character) ||
           character == ':' || character == '@';
}

[[nodiscard]] bool is_baseline_uri(std::string_view value) {
    if (!has_valid_scheme(value)) return false;
    std::size_t index = value.find(':') + 1;

    if (value.substr(index).starts_with("//")) {
        index += 2;
        const auto authority_start = index;
        auto at = std::string_view::npos;
        for (std::size_t cursor = index; cursor < value.size(); ++cursor) {
            if (value[cursor] == '@') {
                at = cursor;
                break;
            }
            if (value[cursor] == '/' || value[cursor] == '?' || value[cursor] == '#') break;
        }
        if (at != std::string_view::npos) {
            for (std::size_t cursor = authority_start; cursor < at; ++cursor) {
                const auto character = static_cast<unsigned char>(value[cursor]);
                if (character == '[' || character == ']') return false;
                if (character == '%') {
                    if (cursor + 2 >= at ||
                        !is_ascii_hex(static_cast<unsigned char>(value[cursor + 1])) ||
                        !is_ascii_hex(static_cast<unsigned char>(value[cursor + 2]))) {
                        return false;
                    }
                    cursor += 2;
                } else if (!is_uri_unreserved(character) &&
                           !is_uri_sub_delimiter(character) && character != ':') {
                    return false;
                }
            }
            index = at + 1;
        }

        if (index < value.size() && value[index] == '[') {
            ++index;
            while (index < value.size() && value[index] != ']') ++index;
            if (index == value.size()) return false;
            ++index;
        } else {
            while (index < value.size()) {
                const auto character = static_cast<unsigned char>(value[index]);
                if (character == '/' || character == '?' || character == '#' || character == ':') {
                    break;
                }
                if (character < 0x80 &&
                    !is_uri_unreserved(character) && !is_uri_sub_delimiter(character)) {
                    return false;
                }
                ++index;
            }
        }
        if (index < value.size() && value[index] == ':') {
            ++index;
            while (index < value.size()) {
                const auto character = static_cast<unsigned char>(value[index]);
                if (character == '/' || character == '?' || character == '#') break;
                if (!is_ascii_digit(character)) return false;
                ++index;
            }
        }
    }

    while (index < value.size()) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character == '%') {
            if (index + 2 >= value.size() ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 1])) ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (character > 0x7f ||
            (!is_uri_path_character(character) && character != '/' &&
             character != '?' && character != '#')) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] bool is_baseline_iri(std::string_view value) {
    if (!has_valid_scheme(value)) return false;
    const auto colon = value.find(':');
    const auto scheme = value.substr(0, colon);
    const auto remainder = value.substr(colon + 1);
    if (remainder.empty()) return false;
    if (scheme == "http" || scheme == "https" || scheme == "ftp" ||
        scheme == "ws" || scheme == "wss") {
        if (!remainder.starts_with("//")) return false;
        const auto host_start = colon + 3;
        const auto host_end = value.find_first_of("/?#", host_start);
        if (host_start == value.size() || host_end == host_start) return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7f || character == '\\') return false;
    }
    return true;
}

[[nodiscard]] bool is_uri_format(std::string_view value, bool allow_unicode) {
    return allow_unicode ? is_baseline_iri(value) : is_baseline_uri(value);
}

[[nodiscard]] bool is_uri_reference_format(std::string_view value, bool allow_unicode) {
    if (!allow_unicode) {
        constexpr std::string_view forbidden = "[]{}<>^`|";
        for (const unsigned char character : value) {
            if (character > 0x7f || character <= 0x20 || character == 0x7f ||
                character == '\\' ||
                forbidden.find(static_cast<char>(character)) != std::string_view::npos) {
                return false;
            }
        }
        return true;
    }

    if (has_forbidden_reference_character(value, false) ||
        !has_valid_percent_encoding(value)) {
        return false;
    }
    if (value.empty()) return true;
    if (has_valid_scheme(value)) return is_uri_format(value, true);
    if (value.size() >= 3 && is_ascii_alpha(static_cast<unsigned char>(value.front()))) {
        const auto slash_pair = value.find("//");
        const auto colon = value.find(':');
        if (slash_pair != std::string_view::npos && colon == std::string_view::npos &&
            slash_pair > 0 && value[slash_pair - 1] != '/') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_json_pointer(std::string_view value) {
    if (value.empty()) return true;
    if (value.front() != '/') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '~') continue;
        if (index + 1 >= value.size() ||
            (value[index + 1] != '0' && value[index + 1] != '1')) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] bool is_json_pointer_uri_fragment(std::string_view value) {
    if (value.empty() || value.front() != '#') return false;
    const auto pointer = value.substr(1);
    if (!is_json_pointer(pointer) || !has_valid_percent_encoding(pointer)) return false;
    for (const unsigned char character : pointer) {
        if (character <= 0x20 || character >= 0x7f) return false;
    }
    return true;
}

[[nodiscard]] bool is_relative_json_pointer(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size() && is_ascii_digit(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    if (index == 0 || (index > 1 && value.front() == '0')) return false;
    if (index == value.size()) return true;
    if (value[index] == '#') return index + 1 == value.size();
    return is_json_pointer(value.substr(index));
}

[[nodiscard]] bool is_uri_template_format(std::string_view value) {
    constexpr std::string_view operators = "+#./;?&=,!@|";
    constexpr std::string_view forbidden_literals = "\"'<>\\^`{|}";
    std::size_t index = 0;
    while (index < value.size()) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character == '%') {
            if (index + 2 >= value.size() ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 1])) ||
                !is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (character != '{') {
            if (character <= 0x20 ||
                forbidden_literals.find(static_cast<char>(character)) != std::string_view::npos) {
                return false;
            }
            ++index;
            continue;
        }

        ++index;
        if (index < value.size() &&
            operators.find(value[index]) != std::string_view::npos) {
            ++index;
        }
        bool expect_variable = true;
        while (true) {
            const auto variable_start = index;
            while (index < value.size()) {
                const auto variable_character = static_cast<unsigned char>(value[index]);
                if (is_ascii_alpha(variable_character) || is_ascii_digit(variable_character) ||
                    variable_character == '_') {
                    ++index;
                    continue;
                }
                if (variable_character == '%' && index + 2 < value.size() &&
                    is_ascii_hex(static_cast<unsigned char>(value[index + 1])) &&
                    is_ascii_hex(static_cast<unsigned char>(value[index + 2]))) {
                    index += 3;
                    continue;
                }
                break;
            }
            if (index == variable_start) return false;
            expect_variable = false;

            if (index < value.size() && value[index] == ':') {
                ++index;
                const auto modifier_start = index;
                if (index >= value.size() || value[index] < '1' || value[index] > '9') return false;
                while (index < value.size() && is_ascii_digit(static_cast<unsigned char>(value[index])) &&
                       index - modifier_start < 4) {
                    ++index;
                }
                if (index < value.size() && is_ascii_digit(static_cast<unsigned char>(value[index]))) {
                    return false;
                }
            } else if (index < value.size() && value[index] == '*') {
                ++index;
            }

            if (index < value.size() && value[index] == ',') {
                ++index;
                expect_variable = true;
                continue;
            }
            break;
        }
        if (expect_variable || index >= value.size() || value[index] != '}') return false;
        ++index;
    }
    return true;
}

[[nodiscard]] bool is_public_url_ipv4(std::string_view host) {
    if (!is_ipv4_format(host)) return false;
    std::array<int, 4> octets{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const auto end = host.find('.', start);
        const auto length = end == std::string_view::npos ? host.size() - start : end - start;
        std::from_chars(host.data() + start, host.data() + start + length, octets[index]);
        start = end == std::string_view::npos ? host.size() : end + 1;
    }
    if (octets[0] == 10 || octets[0] == 127 ||
        (octets[0] == 169 && octets[1] == 254) ||
        (octets[0] == 192 && octets[1] == 168) ||
        (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31)) {
        return false;
    }
    return octets[0] >= 1 && octets[0] <= 223 &&
           octets[3] >= 1 && octets[3] <= 254;
}

[[nodiscard]] bool is_url_format(std::string_view value) {
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) return false;
    std::string scheme(value.substr(0, colon));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character - 'A' + 'a')
                   : static_cast<char>(character);
    });
    if (scheme != "http" && scheme != "https" && scheme != "ftp") return false;
    if (!value.substr(colon + 1).starts_with("//")) return false;
    for (const unsigned char character : value) {
        if (character <= 0x20 || character == 0x7f) return false;
    }

    const auto authority_start = colon + 3;
    const auto authority_end = value.find('/', authority_start);
    auto authority = value.substr(
        authority_start,
        authority_end == std::string_view::npos
            ? value.size() - authority_start
            : authority_end - authority_start);
    if (authority.empty() || authority.find_first_of("?#") != std::string_view::npos) return false;
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        if (at == 0 || at + 1 == authority.size()) return false;
        authority.remove_prefix(at + 1);
    }

    auto host = authority;
    if (const auto port = authority.rfind(':'); port != std::string_view::npos) {
        host = authority.substr(0, port);
        const auto digits = authority.substr(port + 1);
        if (digits.size() < 2 || digits.size() > 5 ||
            !std::all_of(digits.begin(), digits.end(), [](unsigned char character) {
                return is_ascii_digit(character);
            })) {
            return false;
        }
    }
    if (host.empty()) return false;
    if (is_ipv4_format(host)) return is_public_url_ipv4(host);

    std::vector<std::string_view> labels;
    std::size_t start = 0;
    while (start <= host.size()) {
        const auto end = host.find('.', start);
        const auto label = host.substr(
            start,
            end == std::string_view::npos ? host.size() - start : end - start);
        if (label.empty() || label.front() == '-' || label.back() == '-') return false;
        for (const unsigned char character : label) {
            if (!is_ascii_alpha(character) && !is_ascii_digit(character) &&
                character != '-' && character < 0x80) {
                return false;
            }
        }
        labels.push_back(label);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (labels.size() < 2) return false;
    const auto top_level = utf8_code_points(labels.back());
    if (top_level.size() < 2 ||
        !std::all_of(top_level.begin(), top_level.end(), [](std::uint32_t point) {
            return point > 0x7f || is_ascii_alpha(static_cast<unsigned char>(point));
        })) {
        return false;
    }
    return true;
}

[[nodiscard]] bool matches_format(FormatKind format, std::string_view value) {
    switch (format) {
    case FormatKind::DateTime:
        return is_date_time_format(value);
    case FormatKind::Date:
        return is_date_format(value);
    case FormatKind::Duration: {
        static const std::regex expression(
            R"(^P((\d+Y(\d+M(\d+D)?)?|\d+M(\d+D)?|\d+D)(T(\d+H(\d+M(\d+S)?)?|\d+M(\d+S)?|\d+S))?|T(\d+H(\d+M(\d+S)?)?|\d+M(\d+S)?|\d+S)|\d+W)$)");
        return std::regex_match(value.begin(), value.end(), expression);
    }
    case FormatKind::Email:
        return is_email_format(value, false);
    case FormatKind::Hostname:
        return is_hostname_format(value);
    case FormatKind::IdnEmail:
        return is_email_format(value, true);
    case FormatKind::IdnHostname:
        return is_idn_hostname_format(value);
    case FormatKind::Ipv4:
        return is_ipv4_format(value);
    case FormatKind::Ipv6:
        return is_ipv6_format(value);
    case FormatKind::IriReference:
        return is_uri_reference_format(value, true);
    case FormatKind::Iri:
        return is_uri_format(value, true);
    case FormatKind::JsonPointerUriFragment:
        return is_json_pointer_uri_fragment(value);
    case FormatKind::JsonPointer:
        return is_json_pointer(value);
    case FormatKind::Regex:
        if (value.empty()) return false;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
            (void)std::regex(std::string(value), std::regex::ECMAScript);
            return true;
        } catch (const std::regex_error&) {
            return false;
        }
#else
        (void)std::regex(std::string(value), std::regex::ECMAScript);
        return true;
#endif
    case FormatKind::RelativeJsonPointer:
        return is_relative_json_pointer(value);
    case FormatKind::Time:
        return is_time_format(value);
    case FormatKind::UriReference:
        return is_uri_reference_format(value, false);
    case FormatKind::UriTemplate:
        return is_uri_template_format(value);
    case FormatKind::Uri:
        return is_uri_format(value, false);
    case FormatKind::Url:
        return is_url_format(value);
    case FormatKind::Uuid: {
        static const std::regex expression(
            R"(^(?:urn:uuid:)?[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}$)");
        return std::regex_match(value.begin(), value.end(), expression);
    }
    }
    return false;
}

[[nodiscard]] std::vector<std::uint32_t> utf8_code_points(std::string_view value) {
    return decode_utf8(value);
}

[[nodiscard]] bool is_combining_modifier(std::uint32_t point) {
    return (point >= 0x0300 && point <= 0x036f) ||
           (point >= 0x1ab0 && point <= 0x1aff) ||
           (point >= 0x1dc0 && point <= 0x1dff) ||
           (point >= 0xfe20 && point <= 0xfe2f) ||
           (point >= 0xfe00 && point <= 0xfe0f);
}

[[nodiscard]] std::size_t grapheme_count(std::string_view value) {
    const auto points = utf8_code_points(value);
    std::size_t count = 0;
    for (std::size_t index = 0; index < points.size();) {
        const auto first = points[index++];
        while (index < points.size() && is_combining_modifier(points[index])) ++index;
        while (index + 1 < points.size() && points[index] == 0x200d) {
            index += 2;
            while (index < points.size() && is_combining_modifier(points[index])) ++index;
        }
        if (first >= 0x1f1e6 && first <= 0x1f1ff && index < points.size() &&
            points[index] >= 0x1f1e6 && points[index] <= 0x1f1ff) {
            ++index;
        }
        ++count;
    }
    return count;
}

[[nodiscard]] bool triggers_typebox_grapheme_fallback(std::uint32_t point) {
    return point > 0xffff ||
           (point >= 0x0300 && point <= 0x036f) ||
           point == 0x200d;
}

[[nodiscard]] bool typebox_min_length_matches(
    std::string_view value,
    std::size_t min_length) {
    if (min_length == 0) return true;
    std::size_t fast_length = 0;
    for (const auto point : utf8_code_points(value)) {
        if (triggers_typebox_grapheme_fallback(point)) {
            return grapheme_count(value) >= min_length;
        }
        ++fast_length;
        if (fast_length >= min_length) return true;
    }
    return false;
}

[[nodiscard]] bool typebox_max_length_matches(
    std::string_view value,
    std::size_t max_length) {
    std::size_t fast_length = 0;
    for (const auto point : utf8_code_points(value)) {
        if (triggers_typebox_grapheme_fallback(point)) {
            return grapheme_count(value) <= max_length;
        }
        ++fast_length;
        if (fast_length > max_length) return false;
    }
    return true;
}

[[nodiscard]] bool includes_type(const CompiledSchema& schema, JsonType type) {
    return std::find(schema.types.begin(), schema.types.end(), type) != schema.types.end();
}

void validate_value(
    const support::JsonValue& value,
    const CompiledSchema& schema,
    std::string location,
    std::vector<ValidationFailure>& failures);

[[nodiscard]] bool value_satisfies(
    const support::JsonValue& value,
    const CompiledSchema& schema);

void coerce_value(support::JsonValue& value, const CompiledSchema& schema);

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

[[nodiscard]] bool value_satisfies(
    const support::JsonValue& value,
    const CompiledSchema& schema) {
    std::vector<ValidationFailure> failures;
    validate_value(value, schema, "root", failures);
    return failures.empty();
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

[[nodiscard]] support::Expected<support::JsonValue> parse_and_clone_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call) {
    if (!call.arguments_valid) {
        return std::unexpected(malformed_arguments_error(
            tool.name,
            call.argument_error.value_or(std::string{})));
    }
    if (!call.raw_arguments.empty()) {
        auto parsed = support::read_json(call.raw_arguments);
        if (!parsed) {
            return std::unexpected(malformed_arguments_error(
                tool.name,
                parsed.error().detail));
        }
        return *parsed;
    }
    if (call.arguments) {
        return *call.arguments;
    }
    auto parsed = support::read_json("{}");
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

} // namespace

support::Expected<support::JsonValue> prepare_tool_arguments(
    const ai::Tool& tool,
    const ai::ToolCallContent& call) {
    auto arguments = parse_and_clone_arguments(tool, call);
    if (!arguments) {
        return std::unexpected(arguments.error());
    }

    CompilationState compilation_state;
    auto schema = compile_schema(
        tool.parameters,
        "schema",
        0,
        compilation_state,
        CompilationContext{});
    if (!schema) {
        return std::unexpected(preparation_error(tool.name, schema.error().detail));
    }

    coerce_value(*arguments, *schema);
    std::vector<ValidationFailure> failures;
    validate_value(*arguments, *schema, "root", failures);
    if (!failures.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "tool arguments do not satisfy their contract",
            validation_diagnostic(tool.name, failures)));
    }
    return arguments;
}

} // namespace cch::agent
