#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::agent {

constexpr std::size_t kMaxSchemaDepth = 64;
constexpr std::size_t kMaxSchemaNodes = 2048;
constexpr std::size_t kMaxValidationFailures = 8;

/// (the SessionSelectorSearch precedent; issue #487). libstdc++ has no
/// non-throwing `std::regex` constructor: in the exception-enabled build
/// malformed patterns surface as `std::regex_error`, but with exceptions
/// disabled an invalid pattern aborts. This scan rejects the structural
/// errors users commonly type — an unterminated character class, an unmatched
/// or unopened group, or a dangling escape — before any `std::regex` is
/// constructed from an untrusted pattern. A pattern that passes this scan but
/// is still rejected by the compiler remains a documented residual risk in
/// the no-exception build.
[[nodiscard]] inline bool structurally_valid_regex(std::string_view pattern) {
    std::size_t group_depth = 0;
    bool in_character_class = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const char character = pattern[index];
        if (character == '\\') {
            if (index + 1 >= pattern.size()) return false;
            ++index; // the escaped character is literal
            continue;
        }
        if (in_character_class) {
            if (character == ']') in_character_class = false;
            continue;
        }
        switch (character) {
        case '[':
            in_character_class = true;
            break;
        case '(':
            ++group_depth;
            break;
        case ')':
            if (group_depth == 0) return false;
            --group_depth;
            break;
        default:
            break;
        }
    }
    return !in_character_class && group_depth == 0;
}

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

[[nodiscard]] inline bool json_equal(const support::JsonValue& left, const support::JsonValue& right) {
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
        return array->size() == other.size() && std::equal(array->begin(), array->end(), other.begin(), json_equal);
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

[[nodiscard]] inline std::vector<std::uint32_t> decode_utf8(std::string_view value) {
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

[[nodiscard]] inline std::wstring utf8_to_wide(std::string_view value) {
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

/// The tolerant byte-walk decoder the format validators share (the
/// TypeBox-differential semantics: invalid sequences fall back per byte).
[[nodiscard]] inline std::vector<std::uint32_t> utf8_code_points(std::string_view value) { return decode_utf8(value); }

/// The format-assertion dispatch (compile unit stores FormatKind; the
/// validators live in the format-validation unit).
[[nodiscard]] bool matches_format(FormatKind format, std::string_view value);

/// The TypeBox 1.1.38 grapheme-bound checks (the string min/maxLength
/// quirk); defined in the format-validation unit, consumed by validation.
[[nodiscard]] bool typebox_min_length_matches(std::string_view value, std::size_t min_length);
[[nodiscard]] bool typebox_max_length_matches(std::string_view value, std::size_t max_length);

/// The executable-profile seams the entry orchestrator routes through:
/// schema compilation (compile unit), coercion, validation, and the failure
/// diagnostic (validation unit).
[[nodiscard]] support::Expected<CompiledSchema> compile_schema(const support::JsonValue& schema,
        std::string schema_path,
        std::size_t depth,
        CompilationState& state,
        CompilationContext context);

void coerce_value(support::JsonValue& value, const CompiledSchema& schema);

void validate_value(const support::JsonValue& value,
        const CompiledSchema& schema,
        std::string location,
        std::vector<ValidationFailure>& failures);

[[nodiscard]] std::string validation_diagnostic(
        const std::string& tool_name, const std::vector<ValidationFailure>& failures);

} // namespace cch::agent
