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
constexpr std::size_t kDefaultInputSchemaMaxChars = 16'000;

[[nodiscard]] bool is_object(const JsonValue& value) { return value.holds<JsonValue::object_t>(); }

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
    std::string text = description->get_string();
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
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

    const bool any_description = std::any_of(names.begin(), names.end(), [&](const std::string& name) {
        return !description_of(member(properties_ptr != nullptr ? *properties_ptr : schema, name)).empty();
    });
    if (!any_description) {
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
        const std::string description = description_of(member(*properties_ptr, names[i]));
        std::string_view remaining = description;
        while (!remaining.empty()) {
            const std::size_t newline = remaining.find('\n');
            std::string line{remaining.substr(0, newline == std::string_view::npos ? remaining.size() : newline)};
            if (newline != std::string_view::npos)
                remaining = remaining.substr(newline + 1);
            else
                remaining = {};
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            line.erase(line.begin(),
                    std::find_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) == 0; }));
            line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char c) { return std::isspace(c) == 0; })
                               .base(),
                    line.end());
            lines += "\n" + std::string{kIndent} + "// " + line;
        }
        lines += "\n" + std::string{kIndent} + members[i];
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
    std::string identifier;
    for (const char c : name) {
        const bool valid = identifier.empty() ? is_identifier(std::string_view{&c, 1}) : [&] {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
        }();
        identifier += valid ? c : '_';
    }
    return identifier.empty() ? "_" : identifier;
}

std::string schema_to_type(const JsonValue& schema, std::size_t max_chars) {
    SchemaContext context;
    context.root = &schema;
    std::string rendered = to_type(schema, context);
    if (max_chars != 0 && rendered.size() > max_chars) return "unknown";
    return rendered;
}

std::string render_tool_sample(const ai::Tool& tool) {
    const std::string input =
            tool.parameters.holds<JsonValue::object_t>() || tool.parameters.holds<JsonValue::array_t>()
                    ? schema_to_type(tool.parameters, kDefaultInputSchemaMaxChars)
                    : std::string{"unknown"};
    std::string description = tool.description;
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    description.erase(description.begin(), std::find_if(description.begin(), description.end(), not_space));
    description.erase(std::find_if(description.rbegin(), description.rend(), not_space).base(), description.end());
    const std::string declaration = "declare const tools: { " + to_codemode_identifier(tool.name) + "(args: " + input +
                                    "): Promise<unknown>; };";
    return description + "\n\ncodemode tool declaration:\n```ts\n" + declaration + "\n```";
}

} // namespace cch::codemode
