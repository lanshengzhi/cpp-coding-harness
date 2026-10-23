#include <cch/codemode/Declarations.hpp>

#include "support/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::codemode {

namespace {

using support::JsonValue;

constexpr std::string_view kIndent = "  ";
constexpr std::size_t kMaxRefExpansions = 32;
/// Longest property tag; longer ones (large defaults, long patterns) are left out
/// (pi `MAX_TAG_CHARS`).
constexpr std::size_t kMaxTagChars = 80;
/// Schema keywords shown as `@keyword value` property tags, in pi's order.
constexpr std::string_view kTagKeywords[] = {
        "minimum",
        "exclusiveMinimum",
        "maximum",
        "exclusiveMaximum",
        "multipleOf",
        "minLength",
        "maxLength",
        "pattern",
        "format",
        "minItems",
        "maxItems",
};
constexpr std::string_view kLowerBounds[] = {"minimum", "exclusiveMinimum", "minLength", "minItems"};
constexpr std::string_view kUpperBounds[] = {"maximum", "exclusiveMaximum", "maxLength", "maxItems"};

[[nodiscard]] bool is_object(const JsonValue& value) { return value.holds<JsonValue::object_t>(); }

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), not_space);
    const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return begin < end ? std::string{begin, end} : std::string{};
}

[[nodiscard]] const JsonValue* member(const JsonValue& object, std::string_view key) {
    if (!is_object(object)) return nullptr;
    const auto& fields = object.get_object();
    const auto found = fields.find(std::string{key});
    return found == fields.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string json_literal(const JsonValue& value) {
    auto written = support::write_json(value);
    return written ? *written : std::string{"unknown"};
}

[[nodiscard]] bool is_identifier(std::string_view name) {
    if (name.empty()) return false;
    const auto is_start = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
    };
    const auto is_part = [&](char c) { return is_start(c) || (c >= '0' && c <= '9'); };
    if (!is_start(name.front())) return false;
    return std::all_of(name.begin() + 1, name.end(), is_part);
}

[[nodiscard]] std::string property_key(std::string_view name) {
    return is_identifier(name) ? std::string{name} : json_literal(JsonValue{std::string{name}});
}

[[nodiscard]] std::string union_types(std::vector<std::string> types) {
    std::vector<std::string> unique;
    for (auto& type : types) {
        if (std::find(unique.begin(), unique.end(), type) == unique.end()) {
            unique.push_back(std::move(type));
        }
    }
    if (std::find(unique.begin(), unique.end(), "unknown") != unique.end()) return "unknown";
    if (unique.empty()) return "never";
    std::string joined;
    for (std::size_t i = 0; i < unique.size(); ++i) {
        if (i != 0) joined += " | ";
        joined += unique[i];
    }
    return joined;
}

struct SchemaContext {
    const JsonValue* root{nullptr};
    std::set<std::string> resolving;
    std::size_t expansions{0};
    /// Whether property comments include constraint tags (pi `SchemaContext.tags`).
    bool tags{true};
};

[[nodiscard]] const JsonValue* resolve_ref(std::string_view ref, const JsonValue& root) {
    if (ref != "#" && !ref.starts_with("#/")) return nullptr;
    const JsonValue* current = &root;
    std::string_view path = ref;
    if (path.size() > 2) path = path.substr(2);
    std::size_t start = 0;
    while (start <= path.size() && !path.empty()) {
        const std::size_t slash = path.find('/', start);
        std::string segment{
                path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start)};
        std::string decoded;
        for (std::size_t i = 0; i < segment.size(); ++i) {
            if (segment[i] == '~' && i + 1 < segment.size()) {
                if (segment[i + 1] == '1') {
                    decoded += '/';
                    ++i;
                    continue;
                }
                if (segment[i + 1] == '0') {
                    decoded += '~';
                    ++i;
                    continue;
                }
            }
            decoded += segment[i];
        }
        if (!is_object(*current)) return nullptr;
        const auto& fields = current->get_object();
        const auto found = fields.find(decoded);
        if (found == fields.end()) return nullptr;
        current = &found->second;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
        if (start > path.size()) break;
    }
    return current;
}

[[nodiscard]] std::string to_type(const JsonValue& schema, SchemaContext& context);

[[nodiscard]] std::string array_type(const JsonValue& schema, SchemaContext& context) {
    if (const JsonValue* items = member(schema, "items"); items != nullptr && !items->holds<JsonValue::array_t>()) {
        return "Array<" + to_type(*items, context) + ">";
    }
    const JsonValue* tuple = nullptr;
    if (const JsonValue* prefix = member(schema, "prefixItems");
            prefix != nullptr && prefix->holds<JsonValue::array_t>()) {
        tuple = prefix;
    } else if (const JsonValue* items = member(schema, "items");
            items != nullptr && items->holds<JsonValue::array_t>()) {
        tuple = items;
    }
    if (tuple != nullptr && !tuple->get_array().empty()) {
        std::string rendered = "[";
        const auto& entries = tuple->get_array();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) rendered += ", ";
            rendered += to_type(entries[i], context);
        }
        rendered += "]";
        return rendered;
    }
    return "unknown[]";
}

[[nodiscard]] std::string description_of(const JsonValue* property) {
    if (property == nullptr || !is_object(*property)) return {};
    const JsonValue* description = member(*property, "description");
    if (description == nullptr || !description->holds<std::string>()) return {};
    return trimmed(description->get_string());
}

/// Characters that end a `//` comment (pi `LINE_TERMINATORS`).
[[nodiscard]] bool is_line_terminator(std::string_view unit) {
    return unit == "\r" || unit == "\n" || unit == "\xe2\x80\xa8" || unit == "\xe2\x80\xa9";
}

/// Split on line terminators, keeping empty lines (doc comments keep them).
[[nodiscard]] std::vector<std::string> split_description_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const std::size_t width = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xe ? 3 : 4;
        const std::string_view unit = text.substr(index, width);
        if (is_line_terminator(unit)) {
            lines.push_back(current);
            current.clear();
        } else {
            current += unit;
        }
        index += width;
    }
    lines.push_back(current);
    return lines;
}

/// Split on pi's line terminators and trim each line, dropping empties.
[[nodiscard]] std::vector<std::string> comment_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const std::size_t width = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xe ? 3 : 4;
        const std::string_view unit = text.substr(index, width);
        if (is_line_terminator(unit)) {
            lines.push_back(trimmed(current));
            current.clear();
        } else {
            current += unit;
        }
        index += width;
    }
    lines.push_back(trimmed(current));
    std::erase_if(lines, [](const std::string& line) { return line.empty(); });
    return lines;
}

/// Replace pi's line terminators with their `\uXXXX` escapes (tag text).
[[nodiscard]] std::string escape_terminators(std::string_view text) {
    std::string escaped;
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const std::size_t width = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xe ? 3 : 4;
        const std::string_view unit = text.substr(index, width);
        if (unit == "\r")
            escaped += "\\u000d";
        else if (unit == "\n")
            escaped += "\\u000a";
        else if (unit == "\xe2\x80\xa8")
            escaped += "\\u2028";
        else if (unit == "\xe2\x80\xa9")
            escaped += "\\u2029";
        else
            escaped += unit;
        index += width;
    }
    return escaped;
}

/// Constraints a property's TypeScript type cannot express, as JSDoc-style tags
/// (pi `constraintTags`): `@integer @minimum 1 @maximum 100 @default 20`.
[[nodiscard]] std::string constraint_tags(const JsonValue* property) {
    if (property == nullptr || !is_object(*property)) return {};
    const JsonValue* variants = member(*property, "anyOf");
    if (variants == nullptr) variants = member(*property, "oneOf");
    // Optional fields such as Pydantic's `int | None` keep their constraints in
    // the non-null branch; the branch's constraints apply with the property's.
    std::vector<const JsonValue*> schemas{property};
    if (variants != nullptr && variants->holds<JsonValue::array_t>()) {
        std::vector<const JsonValue*> non_null;
        for (const auto& variant : variants->get_array()) {
            const JsonValue* type = member(variant, "type");
            if (type == nullptr || !type->holds<std::string>() || type->get_string() != "null") {
                non_null.push_back(&variant);
            }
        }
        if (non_null.size() == 1) schemas.push_back(non_null.front());
    }
    // Draft-04 and OpenAPI 3.0 make `minimum`/`maximum` exclusive with a boolean
    // `exclusiveMinimum`/`exclusiveMaximum`.
    std::vector<JsonValue::object_t> normalized;
    normalized.reserve(schemas.size());
    for (const JsonValue* schema : schemas) {
        JsonValue::object_t copy = schema->get_object();
        if (const auto it = copy.find("exclusiveMinimum");
                it != copy.end() && it->second.holds<bool>() && it->second.get_boolean()) {
            const auto minimum = copy.find("minimum");
            const std::optional<JsonValue> value =
                    minimum != copy.end() ? std::optional<JsonValue>{minimum->second} : std::nullopt;
            copy.erase("minimum");
            if (value.has_value())
                copy["exclusiveMinimum"] = *value;
            else
                copy.erase("exclusiveMinimum");
        } else if (const auto it = copy.find("exclusiveMinimum"); it != copy.end() && it->second.holds<bool>()) {
            copy.erase("exclusiveMinimum");
        }
        if (const auto it = copy.find("exclusiveMaximum");
                it != copy.end() && it->second.holds<bool>() && it->second.get_boolean()) {
            const auto maximum = copy.find("maximum");
            const std::optional<JsonValue> value =
                    maximum != copy.end() ? std::optional<JsonValue>{maximum->second} : std::nullopt;
            copy.erase("maximum");
            if (value.has_value())
                copy["exclusiveMaximum"] = *value;
            else
                copy.erase("exclusiveMaximum");
        } else if (const auto it = copy.find("exclusiveMaximum"); it != copy.end() && it->second.holds<bool>()) {
            copy.erase("exclusiveMaximum");
        }
        normalized.push_back(std::move(copy));
    }

    std::vector<std::string> tags;
    const auto is_integer = [](const JsonValue::object_t& schema) {
        const auto it = schema.find("type");
        if (it == schema.end()) return false;
        if (it->second.holds<std::string>()) return it->second.get_string() == "integer";
        if (it->second.holds<JsonValue::array_t>()) {
            for (const auto& entry : it->second.get_array())
                if (entry.holds<std::string>() && entry.get_string() == "integer") return true;
        }
        return false;
    };
    if (std::any_of(normalized.begin(), normalized.end(), is_integer)) tags.push_back("@integer");
    for (const auto keyword : kTagKeywords) {
        const bool lower =
                std::find(std::begin(kLowerBounds), std::end(kLowerBounds), keyword) != std::end(kLowerBounds);
        const bool upper =
                std::find(std::begin(kUpperBounds), std::end(kUpperBounds), keyword) != std::end(kUpperBounds);
        if (lower || upper) {
            std::optional<double> bound;
            for (const auto& schema : normalized) {
                const auto it = schema.find(std::string{keyword});
                if (it == schema.end() || !it->second.holds<double>()) continue;
                const double value = it->second.get_number();
                bound = !bound.has_value() ? value : (lower ? std::max(*bound, value) : std::min(*bound, value));
            }
            if (!bound.has_value()) continue;
            tags.push_back(std::format("@{} {}", keyword, json_literal(JsonValue{*bound})));
            continue;
        }
        std::set<std::string> seen;
        for (const auto& schema : normalized) {
            const auto it = schema.find(std::string{keyword});
            if (it == schema.end()) continue;
            if (!it->second.holds<double>() && !it->second.holds<std::string>()) continue;
            const std::string rendered =
                    it->second.holds<std::string>() ? it->second.get_string() : json_literal(it->second);
            if (seen.insert(rendered).second) tags.push_back(std::format("@{} {}", keyword, rendered));
        }
    }
    if (std::any_of(normalized.begin(), normalized.end(), [](const JsonValue::object_t& schema) {
            const auto it = schema.find("uniqueItems");
            return it != schema.end() && it->second.holds<bool>() && it->second.get_boolean();
        })) {
        tags.push_back("@uniqueItems");
    }
    // Annotations, unlike constraints, come from the property itself.
    if (const JsonValue* default_value = member(*property, "default");
            default_value != nullptr && !default_value->holds<JsonValue::null_t>()) {
        tags.push_back("@default " + json_literal(*default_value));
    }
    if (const JsonValue* deprecated = member(*property, "deprecated");
            deprecated != nullptr && deprecated->holds<bool>() && deprecated->get_boolean()) {
        tags.push_back("@deprecated");
    }
    std::string joined;
    for (const auto& tag : tags) {
        if (tag.size() > kMaxTagChars) continue;
        if (!joined.empty()) joined += " ";
        joined += escape_terminators(tag);
    }
    return joined;
}

[[nodiscard]] std::string object_type(const JsonValue& schema, SchemaContext& context) {
    const JsonValue* properties_ptr = member(schema, "properties");
    if (properties_ptr != nullptr && !is_object(*properties_ptr)) properties_ptr = nullptr;
    static const JsonValue::object_t kEmptyProperties{};
    const JsonValue::object_t& properties = properties_ptr != nullptr ? properties_ptr->get_object() : kEmptyProperties;

    std::set<std::string> required;
    if (const JsonValue* required_ptr = member(schema, "required");
            required_ptr != nullptr && required_ptr->holds<JsonValue::array_t>()) {
        for (const auto& entry : required_ptr->get_array()) {
            if (entry.holds<std::string>()) required.insert(entry.get_string());
        }
    }

    std::vector<std::string> names;
    names.reserve(properties.size());
    for (const auto& [name, _] : properties)
        names.push_back(name);
    std::sort(names.begin(), names.end());

    std::vector<std::string> members;
    members.reserve(names.size() + 1);
    for (const auto& name : names) {
        const JsonValue* property = member(properties_ptr != nullptr ? *properties_ptr : schema, name);
        const std::string optional = required.contains(name) ? "" : "?";
        members.push_back(property_key(name) + optional + ": " +
                          (property != nullptr ? to_type(*property, context) : std::string{"unknown"}) + ";");
    }
    const JsonValue* additional = member(schema, "additionalProperties");
    if (additional != nullptr && !(additional->holds<bool>() && !additional->get_boolean())) {
        const std::string type =
                (additional->holds<bool>() && additional->get_boolean()) ? "unknown" : to_type(*additional, context);
        members.push_back("[key: string]: " + type + ";");
    } else if (additional == nullptr && names.empty()) {
        members.push_back("[key: string]: unknown;");
    }
    if (members.empty()) return "{}";

    std::vector<std::vector<std::string>> comments;
    comments.reserve(names.size());
    bool any_comment = false;
    for (const auto& name : names) {
        const JsonValue* property = member(properties_ptr != nullptr ? *properties_ptr : schema, name);
        std::vector<std::string> lines = comment_lines(description_of(property));
        if (context.tags) {
            const std::string tags = constraint_tags(property);
            if (!tags.empty()) lines.push_back(tags);
        }
        any_comment = any_comment || !lines.empty();
        comments.push_back(std::move(lines));
    }
    if (!any_comment) {
        std::string inline_object = "{ ";
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i != 0) inline_object += " ";
            inline_object += members[i];
        }
        inline_object += " }";
        return inline_object;
    }

    std::string lines = "{";
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (const auto& line : comments[i])
            lines += "\n" + std::string{kIndent} + "// " + line;
        std::string member = members[i];
        // A nested object's newlines keep the enclosing indentation.
        std::size_t newline = member.find('\n');
        while (newline != std::string::npos) {
            member.replace(newline, 1, "\n" + std::string{kIndent});
            newline = member.find('\n', newline + 1 + kIndent.size());
        }
        lines += "\n" + std::string{kIndent} + member;
    }
    for (std::size_t i = names.size(); i < members.size(); ++i) {
        lines += "\n" + std::string{kIndent} + members[i];
    }
    lines += "\n}";
    return lines;
}

[[nodiscard]] std::string to_type(const JsonValue& schema, SchemaContext& context) {
    if (schema.holds<bool>()) return schema.get_boolean() ? "unknown" : "never";
    if (!is_object(schema)) return "unknown";

    if (const JsonValue* ref = member(schema, "$ref"); ref != nullptr && ref->holds<std::string>()) {
        const std::string reference = ref->get_string();
        if (context.resolving.contains(reference) || context.expansions >= kMaxRefExpansions) return "unknown";
        const JsonValue* target = context.root != nullptr ? resolve_ref(reference, *context.root) : nullptr;
        if (target == nullptr) return "unknown";
        ++context.expansions;
        context.resolving.insert(reference);
        std::string rendered = to_type(*target, context);
        context.resolving.erase(reference);
        return rendered;
    }

    if (const JsonValue* constant = member(schema, "const"); constant != nullptr) return json_literal(*constant);
    if (const JsonValue* enumeration = member(schema, "enum");
            enumeration != nullptr && enumeration->holds<JsonValue::array_t>()) {
        std::vector<std::string> types;
        for (const auto& value : enumeration->get_array())
            types.push_back(json_literal(value));
        return union_types(std::move(types));
    }

    const JsonValue* variants = member(schema, "anyOf");
    if (variants == nullptr || !variants->holds<JsonValue::array_t>()) variants = member(schema, "oneOf");
    if (variants != nullptr && variants->holds<JsonValue::array_t>()) {
        std::vector<std::string> types;
        for (const auto& variant : variants->get_array())
            types.push_back(to_type(variant, context));
        return union_types(std::move(types));
    }
    if (const JsonValue* all_of = member(schema, "allOf"); all_of != nullptr && all_of->holds<JsonValue::array_t>()) {
        std::vector<std::string> parts;
        for (const auto& part : all_of->get_array()) {
            std::string rendered = to_type(part, context);
            if (rendered != "unknown") parts.push_back(std::move(rendered));
        }
        if (parts.empty()) return "unknown";
        std::string joined;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) joined += " & ";
            joined += parts[i].find(" | ") != std::string::npos ? "(" + parts[i] + ")" : parts[i];
        }
        return joined;
    }

    const JsonValue* type = member(schema, "type");
    if (type != nullptr && type->holds<JsonValue::array_t>()) {
        std::vector<std::string> types;
        for (const auto& entry : type->get_array()) {
            if (!entry.holds<std::string>()) continue;
            JsonValue copy = schema;
            copy.get_object()["type"] = entry;
            types.push_back(to_type(copy, context));
        }
        return union_types(std::move(types));
    }
    const std::string type_name = type != nullptr && type->holds<std::string>() ? type->get_string() : std::string{};
    if (type_name == "string") return "string";
    if (type_name == "number" || type_name == "integer") return "number";
    if (type_name == "boolean") return "boolean";
    if (type_name == "null") return "null";
    if (type_name == "array") return array_type(schema, context);
    if (type_name == "object") return object_type(schema, context);
    if (type_name.empty()) {
        if (member(schema, "properties") != nullptr || member(schema, "additionalProperties") != nullptr ||
                member(schema, "required") != nullptr) {
            return object_type(schema, context);
        }
        if (member(schema, "items") != nullptr || member(schema, "prefixItems") != nullptr) {
            return array_type(schema, context);
        }
        return "unknown";
    }
    return "unknown";
}

} // namespace

std::string to_codemode_identifier(std::string_view name) {
    // pi iterates the string by code point (JS `for...of`), so one invalid
    // character becomes one `_` — `naïve` -> `na_ve`, not `na__ve`.
    std::string identifier;
    std::size_t index = 0;
    while (index < name.size()) {
        const unsigned char byte = static_cast<unsigned char>(name[index]);
        const std::size_t width = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xe ? 3 : 4;
        const std::string_view unit = name.substr(index, width);
        const bool valid = unit.size() == 1 && (identifier.empty() ? is_identifier(unit) : [&] {
            const char c = unit.front();
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
        }());
        identifier += valid ? std::string{unit} : std::string{"_"};
        index += width;
    }
    return identifier.empty() ? "_" : identifier;
}

std::string schema_to_type(const JsonValue& schema, std::size_t max_chars) {
    SchemaContext context;
    context.root = &schema;
    std::string rendered = to_type(schema, context);
    if (max_chars == 0 || rendered.size() <= max_chars) return rendered;
    // Over budget: render again without the constraint tags, then give up.
    context.tags = false;
    std::string untagged = to_type(schema, context);
    return untagged.size() <= max_chars ? untagged : std::string{"unknown"};
}

std::string render_tool_declaration(const ai::Tool& tool, std::size_t input_max_chars) {
    const std::string input =
            tool.parameters.holds<JsonValue::object_t>() || tool.parameters.holds<JsonValue::array_t>()
                    ? schema_to_type(tool.parameters, input_max_chars)
                    : std::string{"unknown"};
    const std::string output = tool.output_schema.has_value()
                                       ? schema_to_type(*tool.output_schema, kDefaultInputSchemaMaxChars)
                                       : std::string{"unknown"};
    return to_codemode_identifier(tool.name) + "(args: " + input + "): Promise<" + output + ">;";
}

std::string render_tool_sample(const ai::Tool& tool) {
    const std::string description = trimmed(tool.description);
    return description + "\n\ncodemode tool declaration:\n```ts\ndeclare const tools: { " +
           render_tool_declaration(tool) + " };\n```";
}

namespace {

/// pi `docComment`: a description as a JSDoc comment at `indent`, multi-line
/// descriptions rendered as one ` * line` per line; empty when there is none.
[[nodiscard]] std::string doc_comment(std::string_view description, std::string_view indent) {
    const std::string text = trimmed(description);
    if (text.empty()) return {};
    std::string escaped;
    for (std::size_t index = 0; index < text.size();) {
        if (text.compare(index, 2, "*/") == 0) {
            escaped += "*\\/";
            index += 2;
            continue;
        }
        escaped += text[index++];
    }
    const std::vector<std::string> lines = split_description_lines(escaped);
    if (lines.size() == 1) return std::string{indent} + "/** " + lines[0] + " */\n";
    std::string rendered = std::string{indent} + "/**";
    for (const auto& line : lines) {
        rendered += "\n" + std::string{indent} + " *";
        if (!line.empty()) rendered += " " + line;
    }
    rendered += "\n" + std::string{indent} + " */\n";
    return rendered;
}

} // namespace

std::string render_declarations(
        const std::vector<ai::Tool>& tools, const std::vector<CodemodeGlobalDeclaration>& globals) {
    constexpr std::string_view kMemberIndent = "  ";
    std::vector<std::string> sections;
    if (!tools.empty()) {
        std::string block = "declare const tools: {";
        for (const auto& tool : tools) {
            block += "\n" + doc_comment(tool.description, kMemberIndent) + std::string{kMemberIndent} +
                     render_tool_declaration(tool);
        }
        block += "\n};";
        sections.push_back(std::move(block));
    }
    // pi collects namespaced globals in a map and emits those blocks after the
    // plain `declare function` statements, in first-appearance order.
    std::vector<std::string> namespace_order;
    std::map<std::string, std::vector<std::string>> namespace_members;
    for (const auto& global : globals) {
        const auto dot = global.name.find('.');
        if (dot == std::string::npos) {
            sections.push_back(
                    doc_comment(global.description, "") + "declare function " + global.name + global.signature + ";");
            continue;
        }
        const std::string name_space = global.name.substr(0, dot);
        if (!namespace_members.contains(name_space)) namespace_order.push_back(name_space);
        namespace_members[name_space].push_back(doc_comment(global.description, kMemberIndent) +
                                                std::string{kMemberIndent} + global.name.substr(dot + 1) +
                                                global.signature + ";");
    }
    for (const auto& name_space : namespace_order) {
        std::string block = "declare const " + name_space + ": {";
        for (const auto& member : namespace_members[name_space])
            block += "\n" + member;
        block += "\n};";
        sections.push_back(std::move(block));
    }
    std::string rendered;
    for (const auto& section : sections) {
        if (!rendered.empty()) rendered += "\n\n";
        rendered += section;
    }
    return rendered;
}

} // namespace cch::codemode
