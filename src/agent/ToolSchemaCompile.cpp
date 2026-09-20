#include "ToolArgumentDetail.hpp"

#include "ToolArgumentPreparation.hpp"

#include "support/BoundedText.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::agent {
namespace {

[[nodiscard]] std::string child_path(std::string_view base, std::string_view key) {
    if (base == "root") {
        return std::string(key);
    }
    std::string result(base);
    result.push_back('.');
    result.append(key);
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


} // namespace

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
        // libstdc++ cannot report a malformed pattern without exceptions
        // (issue #487): reject structurally invalid patterns before any
        // `std::regex` construction, same as SessionSelectorSearch.
        if (!structurally_valid_regex(*pattern)) {
            return std::unexpected(schema_compile_error(
                "unsupported Tool Argument Contract",
                child_path(schema_path, "pattern") +
                    " cannot be enforced as a regular expression"));
        }
        compiled.pattern.emplace(utf8_to_wide(*pattern), std::regex::ECMAScript);
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

} // namespace cch::agent
