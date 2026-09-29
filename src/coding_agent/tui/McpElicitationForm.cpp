#include "coding_agent/tui/McpElicitationForm.hpp"

#include "support/Json.hpp"

#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using support::JsonValue;
namespace bound = mcp_form_bound;

[[nodiscard]] const JsonValue* member(const JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<JsonValue::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto found = object->find(std::string(key));
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<std::string> read_string(const JsonValue& value, std::string_view key) {
    const auto* found = member(value, key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto* text = found->get_if<std::string>();
    return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
}

/// The declared type, lower-cased, narrowed to the four this build edits.
/// `array`, `object`, and anything a future revision adds are carried as a
/// string field: the user still gets a row to answer in and the value travels
/// as the text they typed. A form that silently dropped a field the Upstream
/// marked required would be answering a question nobody was asked.
[[nodiscard]] std::string read_type(const JsonValue& schema) {
    const auto declared = read_string(schema, "type").value_or("string");
    std::string lowered;
    lowered.reserve(declared.size());
    for (const char character : declared) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (lowered == "string" || lowered == "number" || lowered == "integer" || lowered == "boolean") {
        return lowered;
    }
    return "string";
}

/// The ASCII whitespace a pasted value arrives with, so `" 42 "` is the
/// number 42 rather than a number the schema rejects. A string field keeps
/// its text exactly as typed; only the emptiness test and the numeric and
/// boolean parses see the trimmed form.
[[nodiscard]] std::string_view trimmed(std::string_view text) {
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// How many characters `text` has, in Unicode code points: every byte that
/// does not continue a UTF-8 sequence starts one. `minLength` and
/// `maxLength` are character counts, and a byte count would reject a value of
/// non-ASCII text the schema allows. A byte that is not valid UTF-8 counts as
/// one, so malformed text is measured rather than skipped.
[[nodiscard]] std::size_t code_points(std::string_view text) {
    std::size_t count{0};
    for (const char character : text) {
        if ((static_cast<unsigned char>(character) & 0xC0U) != 0x80U) {
            count += 1;
        }
    }
    return count;
}

[[nodiscard]] std::string cut(std::string_view text, std::size_t columns) {
    auto truncated = cch::tui::truncate_text(text, columns);
    if (truncated) {
        return *truncated;
    }
    // A label the truncator cannot measure is cut by bytes rather than shown
    // whole: the bound is containment, and exceeding it is the failure the
    // bound exists to prevent.
    return std::string(text.substr(0, columns));
}

[[nodiscard]] std::optional<std::string> maybe_cut(std::string_view text, std::size_t columns) {
    if (text.empty()) {
        return std::nullopt;
    }
    return cut(text, columns);
}

/// Whether `text` nests objects or arrays deeper than `limit`, counting only
/// brackets outside a JSON string. The JSON reader this build uses is
/// recursive descent, so a schema nested deeper than the stack can hold is a
/// crash, and this textual pre-check is what keeps the reader away from one.
/// It errs towards reporting nesting that is not there, which renders no form
/// rather than overflowing a stack.
[[nodiscard]] bool nests_deeper_than(std::string_view text, std::size_t limit) {
    std::size_t depth{0};
    bool in_string{false};
    bool escaped{false};
    for (const char character : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        switch (character) {
        case '"':
            in_string = true;
            break;
        case '{':
        case '[':
            depth += 1;
            if (depth > limit) {
                return true;
            }
            break;
        case '}':
        case ']':
            if (depth > 0) {
                depth -= 1;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

/// A numeric constraint from the schema, or nothing when it is absent or not
/// a finite number. A constraint this build cannot read validates nothing,
/// which is the safe direction: it cannot reject a value the Upstream asked
/// for.
[[nodiscard]] std::optional<double> read_number(const JsonValue& value, std::string_view key) {
    const auto* found = member(value, key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto* number = found->get_if<double>();
    if (number == nullptr || !std::isfinite(*number)) {
        return std::nullopt;
    }
    return *number;
}

[[nodiscard]] std::optional<std::size_t> read_length(const JsonValue& value, std::string_view key) {
    const auto number = read_number(value, key);
    if (!number || *number < 0.0 || std::trunc(*number) != *number ||
        *number > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*number);
}

[[nodiscard]] McpFieldCoercion invalid(std::string message) {
    return McpFieldCoercion{.outcome = McpFieldOutcome::Invalid, .value = {}, .error = std::move(message)};
}

[[nodiscard]] McpFieldCoercion valued(JsonValue value) {
    return McpFieldCoercion{.outcome = McpFieldOutcome::Value, .value = std::move(value), .error = {}};
}

} // namespace

std::optional<std::string> McpElicitationForm::notice() const {
    if (fault != McpFormSchemaFault::None) {
        switch (fault) {
        case McpFormSchemaFault::TooLarge:
            return "this build renders no fields from a form schema this large, so the answer carries none";
        case McpFormSchemaFault::TooDeep:
            return "this build renders no fields from a form schema nested this deeply, so the answer carries none";
        case McpFormSchemaFault::Unreadable:
            return "the Upstream's form schema is not readable JSON, so the answer carries no fields";
        case McpFormSchemaFault::NotAnObject:
            return "the Upstream's form schema is not a JSON object, so it declares no fields";
        case McpFormSchemaFault::None:
            break;
        }
        return "the Upstream's form schema could not be read";
    }
    if (declared > fields.size()) {
        return "this build renders the first " + std::to_string(bound::kMaxFields) + " of " +
               std::to_string(declared) + " fields, so the answer carries only those";
    }
    return std::nullopt;
}

McpElicitationForm read_form_schema(std::string_view schema) {
    McpElicitationForm form;
    if (schema.size() > bound::kMaxSchemaBytes) {
        form.fault = McpFormSchemaFault::TooLarge;
        return form;
    }
    if (nests_deeper_than(schema, bound::kMaxNesting)) {
        form.fault = McpFormSchemaFault::TooDeep;
        return form;
    }
    auto parsed = support::read_json(schema);
    if (!parsed) {
        form.fault = McpFormSchemaFault::Unreadable;
        return form;
    }
    if (!parsed->holds<JsonValue::object_t>()) {
        form.fault = McpFormSchemaFault::NotAnObject;
        return form;
    }
    if (const auto title = read_string(*parsed, "title"); title) {
        form.title = maybe_cut(*title, bound::kMaxLabelColumns);
    }
    if (const auto description = read_string(*parsed, "description"); description) {
        form.description = maybe_cut(*description, bound::kMaxDescriptionColumns);
    }
    const auto* properties = member(*parsed, "properties");
    const auto* object = properties == nullptr ? nullptr : properties->get_if<JsonValue::object_t>();
    if (object == nullptr) {
        return form; // an object schema with no properties asks for nothing
    }
    form.declared = object->size();
    // `required` is an array of property names. A name that is not a string
    // marks no field, so a malformed entry can only make a field optional.
    std::vector<std::string_view> required_names;
    if (const auto* declared = member(*parsed, "required");
        declared != nullptr && declared->holds<JsonValue::array_t>()) {
        for (const auto& entry : declared->get<JsonValue::array_t>()) {
            if (const auto* name = entry.get_if<std::string>()) {
                required_names.emplace_back(*name);
            }
        }
    }
    for (const auto& [name, schema_field] : *object) {
        if (name.empty()) {
            continue; // an empty property name cannot be an answer's key
        }
        if (form.fields.size() >= bound::kMaxFields) {
            break;
        }
        McpElicitationField field;
        field.name = name;
        field.type = read_type(schema_field);
        field.required = std::ranges::find(required_names, std::string_view{name}) != required_names.end();
        field.title = cut(read_string(schema_field, "title").value_or(name), bound::kMaxLabelColumns);
        if (const auto description = read_string(schema_field, "description"); description) {
            field.description = maybe_cut(*description, bound::kMaxDescriptionColumns);
        }
        if (field.type == "string") {
            if (const auto* choices = member(schema_field, "enum");
                choices != nullptr && choices->holds<JsonValue::array_t>()) {
                for (const auto& choice : choices->get<JsonValue::array_t>()) {
                    if (field.enum_values.size() >= bound::kMaxEnumValues) {
                        break;
                    }
                    if (const auto* text = choice.get_if<std::string>()) {
                        field.enum_values.push_back(cut(*text, bound::kMaxLabelColumns));
                    }
                }
            }
        }
        field.minimum = read_number(schema_field, "minimum");
        field.maximum = read_number(schema_field, "maximum");
        field.min_length = read_length(schema_field, "minLength");
        field.max_length = read_length(schema_field, "maxLength");
        form.fields.push_back(std::move(field));
    }
    return form;
}

McpFieldCoercion coerce_field(const McpElicitationField& field, std::string_view text) {
    const auto value_text = trimmed(text);
    if (value_text.empty()) {
        if (field.required) {
            return invalid(field.title + " is required");
        }
        return McpFieldCoercion{}; // Absent: the answer omits the field
    }
    if (field.type == "number" || field.type == "integer") {
        double number{0.0};
        const auto* const first = value_text.data();
        const auto* const last = value_text.data() + value_text.size();
        const auto parsed = std::from_chars(first, last, number, std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(number)) {
            return invalid(field.title + " must be a " + field.type);
        }
        if (field.type == "integer" && std::trunc(number) != number) {
            return invalid(field.title + " must be a whole number");
        }
        if (field.minimum && number < *field.minimum) {
            return invalid(field.title + " must be at least " + std::to_string(*field.minimum));
        }
        if (field.maximum && number > *field.maximum) {
            return invalid(field.title + " must be at most " + std::to_string(*field.maximum));
        }
        return valued(JsonValue(number));
    }
    if (field.type == "boolean") {
        if (value_text == "true") {
            return valued(JsonValue(true));
        }
        if (value_text == "false") {
            return valued(JsonValue(false));
        }
        return invalid(field.title + " must be true or false");
    }
    // Strings, and every type this build has no editor for, travel as typed.
    const auto length = code_points(text);
    if (field.min_length && length < *field.min_length) {
        return invalid(field.title + " must be at least " + std::to_string(*field.min_length) + " characters");
    }
    if (field.max_length && length > *field.max_length) {
        return invalid(field.title + " must be at most " + std::to_string(*field.max_length) + " characters");
    }
    if (!field.enum_values.empty() &&
        std::ranges::find(field.enum_values, value_text) == field.enum_values.end()) {
        std::string allowed;
        for (const auto& choice : field.enum_values) {
            if (!allowed.empty()) {
                allowed += ", ";
            }
            allowed += choice;
        }
        return invalid(field.title + " must be one of: " + allowed);
    }
    return valued(JsonValue(std::string{text}));
}

} // namespace cch::coding_agent::tui
