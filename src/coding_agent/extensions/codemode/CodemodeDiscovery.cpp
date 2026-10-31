#include "coding_agent/extensions/codemode/CodemodeDiscovery.hpp"

#include "coding_agent/extensions/codemode/CodemodeCatalog.hpp"

#include <cch/codemode/Declarations.hpp>

#include "support/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

using codemode::render_tool_sample;
using codemode::to_codemode_identifier;
using support::JsonValue;

[[nodiscard]] bool is_object(const JsonValue& value) { return value.holds<JsonValue::object_t>(); }

[[nodiscard]] const JsonValue* member(const JsonValue& object, std::string_view key) {
    if (!is_object(object)) return nullptr;
    const auto& fields = object.get_object();
    const auto found = fields.find(std::string{key});
    return found == fields.end() ? nullptr : &found->second;
}

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

std::vector<std::string> tokenize_tool_text(std::string_view text) { return split_terms(text); }

std::string tool_search_document_text(const ai::Tool& tool, const std::optional<ToolNamespace>& name_space) {
    std::vector<std::string> parts;
    parts.push_back(tool.name);
    std::string spaced_name{tool.name};
    std::replace(spaced_name.begin(), spaced_name.end(), '_', ' ');
    parts.push_back(std::move(spaced_name));
    parts.push_back(tool.description);
    schema_text(tool.parameters, parts);
    if (name_space.has_value()) {
        parts.push_back(name_space->name);
        parts.push_back(name_space->description);
        parts.push_back(name_space->instructions);
    }
    return join_non_empty(parts);
}

std::vector<std::string> bm25_rank(
        std::string_view query, const std::vector<std::pair<std::string, std::string>>& documents, std::size_t limit) {
    std::vector<std::string> query_terms;
    for (const auto& term : split_terms(query)) {
        if (std::find(query_terms.begin(), query_terms.end(), term) == query_terms.end()) query_terms.push_back(term);
    }
    if (query_terms.empty() || documents.empty() || limit == 0) return {};

    constexpr double k1 = 1.2;
    constexpr double b = 0.75;
    const std::size_t count = documents.size();
    std::vector<std::map<std::string, double>> term_counts(count);
    std::vector<double> lengths(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        for (const auto& term : split_terms(documents[i].second)) {
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
        if (score > 0) matches.push_back(Scored{documents[i].first, score, i});
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

CodemodeDiscovery::CodemodeDiscovery(std::vector<LoadoutTool> tools) : tools_(std::move(tools)) {
    for (const auto& tool : tools_) {
        // pi `ALL_TOOLS`/`describeTool` samples: the declaration carries the
        // tool's prompt guidelines and its defaulted output schema.
        const CodemodeDeclaration declaration = to_codemode_declaration(tool.definition, tool.guidelines);
        ai::Tool sample;
        sample.name = declaration.name;
        sample.description = declaration.description;
        sample.parameters = declaration.input_schema;
        sample.output_schema = declaration.output_schema;
        samples_.emplace(tool.definition.name, render_tool_sample(sample));
        documents_.emplace_back(tool.definition.name, tool_search_document_text(tool.definition, tool.name_space));
    }
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
    const auto write = [](const JsonValue& value) -> support::Expected<std::string> {
        auto written = support::write_json(value);
        if (!written) return std::unexpected(written.error());
        return support::Expected<std::string>{std::move(*written)};
    };
    const auto results_for = [&](const std::vector<std::string>& ranked) -> support::Expected<std::string> {
        JsonValue::array_t results;
        for (const auto& matched : ranked) {
            results.push_back(JsonValue::object_t{
                    {"name", to_codemode_identifier(matched)},
                    {"description", samples_.contains(matched) ? samples_.at(matched) : std::string{}},
            });
        }
        return write(JsonValue{std::move(results)});
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
                if (!limit_value->holds<double>() || limit_value->get_number() <= 0 ||
                        std::floor(limit_value->get_number()) != limit_value->get_number()) {
                    return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "searchTools() limit must be a positive integer"));
                }
                limit = static_cast<std::size_t>(limit_value->get_number());
            }
            if (const JsonValue* name_space = member(*options, "namespace");
                    name_space != nullptr && !name_space->holds<JsonValue::null_t>()) {
                if (!name_space->holds<std::string>()) {
                    return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "searchTools() namespace must be a string"));
                }
                const std::string query_namespace = name_space->get_string();
                std::vector<std::pair<std::string, std::string>> filtered;
                for (std::size_t index = 0; index < tools_.size(); ++index) {
                    const auto& tool = tools_[index];
                    if (!tool.name_space.has_value() || !is_namespace_name(*tool.name_space, query_namespace)) {
                        continue;
                    }
                    filtered.push_back(documents_[index]);
                }
                return results_for(bm25_rank(query->get_string(), filtered, limit));
            }
        }
        return results_for(bm25_rank(query->get_string(), documents_, limit));
    }
    if (name == "describeTool") {
        const JsonValue* tool_name = arg_at(0);
        if (tool_name == nullptr || !tool_name->holds<std::string>()) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "describeTool() expects a tool name"));
        }
        const std::string requested = tool_name->get_string();
        for (const auto& tool : tools_) {
            if (tool.definition.name == requested || to_codemode_identifier(tool.definition.name) == requested) {
                return write(JsonValue{samples_.at(tool.definition.name)});
            }
        }
        return support::Expected<std::string>{std::string{"null"}};
    }
    if (name == "describeNamespace") {
        const JsonValue* namespace_name = arg_at(0);
        if (namespace_name == nullptr || !namespace_name->holds<std::string>()) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "describeNamespace() expects a namespace name"));
        }
        const std::string requested = namespace_name->get_string();
        const ToolNamespace* found = nullptr;
        JsonValue::array_t names;
        for (const auto& tool : tools_) {
            if (!tool.name_space.has_value() || !is_namespace_name(*tool.name_space, requested)) continue;
            if (found == nullptr) found = &*tool.name_space;
            names.push_back(to_codemode_identifier(tool.definition.name));
        }
        if (found == nullptr) return support::Expected<std::string>{std::string{"null"}};
        JsonValue::object_t described{
                {"name", found->name},
                {"tools", std::move(names)},
        };
        if (!found->description.empty()) described.emplace("description", found->description);
        if (!found->instructions.empty()) described.emplace("instructions", found->instructions);
        return write(JsonValue{std::move(described)});
    }
    return std::unexpected(
            support::make_error(support::ErrorCode::Validation, "unknown codemode global '" + std::string{name} + "'"));
}

} // namespace cch::coding_agent::extensions
