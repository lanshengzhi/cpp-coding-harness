#include "coding_agent/extensions/codemode/CodemodeDiscovery.hpp"

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

namespace cch::coding_agent::extensions {

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

// ── Bm25 ──

const std::set<std::string>& stop_words() {
    static const std::set<std::string> words{"a",
            "an",
            "and",
            "are",
            "as",
            "at",
            "be",
            "by",
            "for",
            "from",
            "in",
            "is",
            "it",
            "of",
            "on",
            "or",
            "that",
            "the",
            "this",
            "to",
            "with"};
    return words;
}

[[nodiscard]] std::string stem(std::string term) {
    const auto ends_with = [&](std::string_view suffix) { return term.ends_with(suffix); };
    if (term.size() > 4 && ends_with("ies")) return term.substr(0, term.size() - 3) + "y";
    if (term.size() > 4 &&
            (ends_with("ches") || ends_with("shes") || ends_with("sses") || ends_with("xes") || ends_with("zes"))) {
        return term.substr(0, term.size() - 2);
    }
    if (term.size() > 3 && ends_with("s") && !ends_with("ss")) return term.substr(0, term.size() - 1);
    return term;
}

/// pi `schemaText`: schema/property descriptions and property names, recursively.
void schema_text(const JsonValue& schema, std::vector<std::string>& parts) {
    if (!is_object(schema)) return;
    if (const JsonValue* description = member(schema, "description");
            description != nullptr && description->holds<std::string>()) {
        parts.push_back(description->get_string());
    }
    if (const JsonValue* properties = member(schema, "properties"); properties != nullptr && is_object(*properties)) {
        for (const auto& [name, property] : properties->get_object()) {
            parts.push_back(name);
            schema_text(property, parts);
        }
    }
    if (const JsonValue* items = member(schema, "items"); items != nullptr) schema_text(*items, parts);
    for (const char* key : {"anyOf", "oneOf", "allOf"}) {
        if (const JsonValue* variants = member(schema, key);
                variants != nullptr && variants->holds<JsonValue::array_t>()) {
            for (const auto& variant : variants->get_array())
                schema_text(variant, parts);
        }
    }
}

[[nodiscard]] std::string join_non_empty(const std::vector<std::string>& parts) {
    std::string joined;
    for (const auto& part : parts) {
        if (part.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        if (!joined.empty()) joined += " ";
        joined += part;
    }
    return joined;
}

[[nodiscard]] std::vector<std::string> split_terms(std::string_view text) {
    // Split at camelCase boundaries and non-alphanumerics, then lowercase and
    // drop stop words (pi `tokenize`).
    std::string spaced;
    spaced.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char current = text[i];
        if (i > 0) {
            const char previous = text[i - 1];
            const bool lower_to_upper =
                    ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9')) && current >= 'A' &&
                    current <= 'Z';
            const bool acronym_end = previous >= 'A' && previous <= 'Z' && current >= 'A' && current <= 'Z' &&
                                     i + 1 < text.size() && text[i + 1] >= 'a' && text[i + 1] <= 'z';
            if (lower_to_upper || acronym_end) spaced += ' ';
        }
        spaced += current;
    }
    std::vector<std::string> terms;
    std::string current;
    const auto flush = [&]() {
        if (current.empty()) return;
        std::string lowered;
        lowered.reserve(current.size());
        for (char c : current) {
            lowered += static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        }
        if (!stop_words().contains(lowered)) terms.push_back(stem(std::move(lowered)));
        current.clear();
    };
    for (char c : spaced) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum)
            current += c;
        else
            flush();
    }
    flush();
    return terms;
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

std::vector<std::string> tokenize_tool_text(std::string_view text) { return split_terms(text); }

std::string tool_search_document_text(const ai::Tool& tool) {
    std::vector<std::string> parts;
    parts.push_back(tool.name);
    std::string spaced_name{tool.name};
    std::replace(spaced_name.begin(), spaced_name.end(), '_', ' ');
    parts.push_back(std::move(spaced_name));
    parts.push_back(tool.description);
    schema_text(tool.parameters, parts);
    return join_non_empty(parts);
}

std::vector<std::string> bm25_rank(std::string_view query, const std::vector<ai::Tool>& tools, std::size_t limit) {
    std::vector<std::string> query_terms;
    for (const auto& term : split_terms(query)) {
        if (std::find(query_terms.begin(), query_terms.end(), term) == query_terms.end()) query_terms.push_back(term);
    }
    if (query_terms.empty() || tools.empty() || limit == 0) return {};

    constexpr double k1 = 1.2;
    constexpr double b = 0.75;
    const std::size_t count = tools.size();
    std::vector<std::map<std::string, double>> term_counts(count);
    std::vector<double> lengths(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        for (const auto& term : split_terms(tool_search_document_text(tools[i]))) {
            term_counts[i][term] += 1;
            lengths[i] += 1;
        }
    }
    double average_length = 0;
    for (const double length : lengths)
        average_length += length;
    average_length = count > 0 ? average_length / static_cast<double>(count) : 0;
    if (average_length == 0) average_length = 1;

    std::map<std::string, double> idf;
    for (const auto& term : query_terms) {
        std::size_t frequency = 0;
        for (const auto& counts : term_counts) {
            if (counts.contains(term)) ++frequency;
        }
        idf[term] = std::log(1 + (static_cast<double>(count) - static_cast<double>(frequency) + 0.5) /
                                         (static_cast<double>(frequency) + 0.5));
    }

    struct Scored {
        std::string name;
        double score;
        std::size_t index;
    };
    std::vector<Scored> matches;
    for (std::size_t i = 0; i < count; ++i) {
        double score = 0;
        for (const auto& term : query_terms) {
            const auto found = term_counts[i].find(term);
            if (found == term_counts[i].end()) continue;
            const double term_count = found->second;
            const double norm = k1 * (1 - b + (b * lengths[i]) / average_length);
            score += idf[term] * ((term_count * (k1 + 1)) / (term_count + norm));
        }
        if (score > 0) matches.push_back(Scored{tools[i].name, score, i});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Scored& left, const Scored& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.index < right.index;
    });
    if (matches.size() > limit) matches.resize(limit);
    std::vector<std::string> names;
    names.reserve(matches.size());
    for (auto& match : matches)
        names.push_back(std::move(match.name));
    return names;
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

bool is_namespace_name(std::string_view namespace_name, std::string_view query) {
    const std::string id = to_codemode_identifier(namespace_name);
    const std::string query_id = to_codemode_identifier(query);
    const auto suffix = [](std::string_view name) -> std::optional<std::string> {
        const std::size_t last = name.rfind("__");
        if (last == std::string_view::npos) return std::nullopt;
        return std::string{name.substr(last + 2)};
    };
    return namespace_name == query || id == query_id || suffix(namespace_name) == query || suffix(id) == query_id;
}

CodemodeDiscovery::CodemodeDiscovery(std::vector<ai::Tool> tools) : tools_(std::move(tools)) {
    for (const auto& tool : tools_)
        samples_.emplace(tool.name, render_tool_sample(tool));
}

support::JsonValue CodemodeDiscovery::globals_json() const {
    JsonValue::array_t globals;
    for (const char* name : {"searchTools", "describeTool", "describeNamespace"}) {
        globals.push_back(JsonValue::object_t{
                {"name", name},
                {"spread", true},
        });
    }
    return JsonValue{std::move(globals)};
}

support::Expected<std::string> CodemodeDiscovery::handle(std::string_view name, const JsonValue& arguments) const {
    const auto arg_at = [&](std::size_t index) -> const JsonValue* {
        if (!arguments.holds<JsonValue::array_t>()) return nullptr;
        const auto& array = arguments.get_array();
        return index < array.size() ? &array[index] : nullptr;
    };
    if (name == "searchTools") {
        const JsonValue* query = arg_at(0);
        if (query == nullptr || !query->holds<std::string>()) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "searchTools() expects a query string"));
        }
        std::size_t limit = kDefaultToolSearchLimit;
        if (const JsonValue* options = arg_at(1); options != nullptr && is_object(*options)) {
            if (const JsonValue* limit_value = member(*options, "limit"); limit_value != nullptr) {
                if (!limit_value->holds<double>() || limit_value->get_number() <= 0) {
                    return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "searchTools() limit must be a positive integer"));
                }
                limit = static_cast<std::size_t>(limit_value->get_number());
            }
        }
        JsonValue::array_t results;
        for (const auto& matched : bm25_rank(query->get_string(), tools_, limit)) {
            results.push_back(JsonValue::object_t{
                    {"name", to_codemode_identifier(matched)},
                    {"description", samples_.contains(matched) ? samples_.at(matched) : std::string{}},
            });
        }
        auto written = support::write_json(JsonValue{std::move(results)});
        return written ? support::Expected<std::string>{std::move(*written)}
                       : support::Expected<std::string>{std::unexpected(written.error())};
    }
    if (name == "describeTool") {
        const JsonValue* tool_name = arg_at(0);
        if (tool_name == nullptr || !tool_name->holds<std::string>()) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "describeTool() expects a tool name"));
        }
        const std::string requested = tool_name->get_string();
        for (const auto& tool : tools_) {
            if (tool.name == requested || to_codemode_identifier(tool.name) == requested) {
                auto written = support::write_json(JsonValue{samples_.at(tool.name)});
                return written ? support::Expected<std::string>{std::move(*written)}
                               : support::Expected<std::string>{std::unexpected(written.error())};
            }
        }
        return support::Expected<std::string>{std::string{"null"}};
    }
    if (name == "describeNamespace") {
        // Pike registers no tool namespaces yet, so no namespace matches.
        return support::Expected<std::string>{std::string{"null"}};
    }
    return std::unexpected(
            support::make_error(support::ErrorCode::Validation, "unknown codemode global '" + std::string{name} + "'"));
}

} // namespace cch::coding_agent::extensions
