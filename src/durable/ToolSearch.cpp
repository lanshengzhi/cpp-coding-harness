#include <cch/durable/ToolSearch.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::durable {
namespace {

using Json = support::JsonValue;

constexpr std::string_view kToolSearchName = "tool_search";
constexpr std::size_t kDefaultLimit = 8;
constexpr double kBm25K1 = 1.2;
constexpr double kBm25B = 0.75;

constexpr auto kStopWords = std::to_array<std::string_view>({"a",
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
        "with"});

bool ascii_alpha(unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
bool ascii_digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }
bool ascii_lower(unsigned char ch) { return ch >= 'a' && ch <= 'z'; }
bool ascii_upper(unsigned char ch) { return ch >= 'A' && ch <= 'Z'; }
unsigned char ascii_to_lower(unsigned char ch) { return ascii_upper(ch) ? ch + ('a' - 'A') : ch; }

std::string stem(std::string term) {
    if (term.size() > 4 && term.ends_with("ies")) return term.substr(0, term.size() - 3) + "y";
    if (term.size() > 4 && (term.ends_with("ches") || term.ends_with("shes") || term.ends_with("sses") ||
                                   term.ends_with("xes") || term.ends_with("zes")))
        return term.substr(0, term.size() - 2);
    if (term.size() > 3 && term.ends_with("s") && !term.ends_with("ss")) return term.substr(0, term.size() - 1);
    return term;
}

/// Frozen durable `tokenize`: ASCII terms, camelCase boundaries, stop-word removal and naive stemming.
std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> terms;
    std::string current;
    const auto flush = [&] {
        if (current.empty() || std::ranges::contains(kStopWords, current)) {
            current.clear();
            return;
        }
        terms.push_back(stem(std::move(current)));
        current.clear();
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto ch = static_cast<unsigned char>(text[index]);
        const bool camel_boundary =
                index > 0 && ascii_upper(ch) &&
                ((ascii_lower(static_cast<unsigned char>(text[index - 1])) ||
                         ascii_digit(static_cast<unsigned char>(text[index - 1]))) ||
                        (ascii_upper(static_cast<unsigned char>(text[index - 1])) && index + 1 < text.size() &&
                                ascii_lower(static_cast<unsigned char>(text[index + 1]))));
        if (camel_boundary) flush();
        if (ascii_alpha(ch) || ascii_digit(ch))
            current.push_back(static_cast<char>(ascii_to_lower(ch)));
        else
            flush();
    }
    flush();
    return terms;
}

void schema_text(const Json& schema, std::vector<std::string>& parts) {
    const auto* object = schema.get_if<Json::object_t>();
    if (object == nullptr) return;
    if (const auto description = object->find("description");
            description != object->end() && description->second.holds<std::string>())
        parts.push_back(description->second.get_string());
    if (const auto properties = object->find("properties"); properties != object->end()) {
        if (const auto* fields = properties->second.get_if<Json::object_t>()) {
            for (const auto& [name, property] : *fields) {
                parts.push_back(name);
                schema_text(property, parts);
            }
        }
    }
    if (const auto items = object->find("items"); items != object->end()) schema_text(items->second, parts);
    for (const auto* key : {"anyOf", "oneOf", "allOf"}) {
        const auto variants = object->find(key);
        if (variants == object->end()) continue;
        if (const auto* values = variants->second.get_if<Json::array_t>())
            for (const auto& variant : *values)
                schema_text(variant, parts);
    }
}

std::string search_text(const ai::Tool& tool) {
    std::vector<std::string> parts{tool.name, tool.name, tool.description};
    std::ranges::replace(parts[1], '_', ' ');
    schema_text(tool.parameters, parts);
    std::string text;
    for (const auto& part : parts) {
        if (part.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        if (!text.empty()) text += ' ';
        text += part;
    }
    return text;
}

struct Document {
    std::string name;
    std::string text;
};

struct Ranked {
    std::string name;
    double score{0.0};
};

/// Frozen durable `rankDocuments`: Okapi BM25 (k1=1.2, b=0.75), stable document-order ties.
std::vector<std::string> rank_documents(
        std::string_view query, const std::vector<Document>& documents, std::size_t limit) {
    std::vector<std::string> query_terms;
    std::set<std::string> seen;
    for (auto& term : tokenize(query))
        if (seen.insert(term).second) query_terms.push_back(std::move(term));
    if (query_terms.empty() || documents.empty() || limit == 0) return {};

    using Counts = std::map<std::string, std::size_t>;
    std::vector<Counts> term_counts;
    std::vector<std::size_t> lengths;
    term_counts.reserve(documents.size());
    lengths.reserve(documents.size());
    for (const auto& document : documents) {
        Counts counts;
        std::size_t length = 0;
        for (const auto& term : tokenize(document.text)) {
            ++counts[term];
            ++length;
        }
        lengths.push_back(length);
        term_counts.push_back(std::move(counts));
    }
    std::size_t total_length = 0;
    for (const auto length : lengths)
        total_length += length;
    const double average_length = documents.empty() || total_length == 0
                                          ? 1.0
                                          : static_cast<double>(total_length) / static_cast<double>(documents.size());
    std::map<std::string, double> idf;
    for (const auto& term : query_terms) {
        const auto frequency =
                std::ranges::count_if(term_counts, [&term](const Counts& counts) { return counts.contains(term); });
        idf.emplace(term,
                std::log(1.0 + (static_cast<double>(documents.size()) - static_cast<double>(frequency) + 0.5) /
                                       (static_cast<double>(frequency) + 0.5)));
    }
    std::vector<Ranked> matches;
    for (std::size_t index = 0; index < documents.size(); ++index) {
        double score = 0.0;
        for (const auto& term : query_terms) {
            const auto found = term_counts[index].find(term);
            if (found == term_counts[index].end()) continue;
            const auto count = static_cast<double>(found->second);
            const double norm =
                    kBm25K1 * (1.0 - kBm25B + kBm25B * static_cast<double>(lengths[index]) / average_length);
            score += idf.at(term) * (count * (kBm25K1 + 1.0)) / (count + norm);
        }
        if (score > 0.0) matches.push_back(Ranked{.name = documents[index].name, .score = score});
    }
    std::ranges::stable_sort(matches, [](const Ranked& left, const Ranked& right) { return left.score > right.score; });
    std::vector<std::string> names;
    for (const auto& match : matches) {
        if (names.size() >= limit) break;
        names.push_back(match.name);
    }
    return names;
}

std::string first_description_line(std::string_view description) {
    const auto end = description.find_first_of("\r\n");
    description = description.substr(0, end);
    const auto begin = description.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    const auto last = description.find_last_not_of(" \t");
    return std::string{description.substr(begin, last - begin + 1)};
}

ai::ToolResultMessage error_result(std::string text) {
    return ai::ToolResultMessage{.tool_name = std::string{kToolSearchName},
            .content = {ai::text_content(std::move(text))},
            .is_error = true};
}

support::Expected<ai::ToolResultMessage> execute(Json arguments, ToolExecutionContext context) {
    const auto* fields = arguments.get_if<Json::object_t>();
    if (fields == nullptr) return error_result("tool_search requires a string query.");
    const auto query = fields->find("query");
    if (query == fields->end() || !query->second.holds<std::string>())
        return error_result("tool_search requires a string query.");
    const std::string& query_text = query->second.get_string();
    if (query_text.find_first_not_of(" \t\r\n") == std::string::npos) return error_result("query must not be empty");
    std::size_t limit = kDefaultLimit;
    if (const auto found = fields->find("limit"); found != fields->end()) {
        if (!found->second.holds<double>()) return error_result("limit must be a positive integer");
        const double value = found->second.get_number();
        if (!std::isfinite(value) || value <= 0.0 || std::trunc(value) != value ||
                value > static_cast<double>(std::numeric_limits<std::size_t>::max()))
            return error_result("limit must be a positive integer");
        limit = static_cast<std::size_t>(value);
    }

    std::vector<ai::Tool> candidates;
    for (const auto& tool : context.tools) {
        if (std::ranges::find(context.deferred_tools, tool.name) == context.deferred_tools.end() ||
                std::ranges::find(context.offered_tools, tool.name) != context.offered_tools.end())
            continue;
        candidates.push_back(tool);
    }
    std::vector<Document> documents;
    documents.reserve(candidates.size());
    for (const auto& candidate : candidates)
        documents.push_back(Document{.name = candidate.name, .text = search_text(candidate)});
    auto loaded = rank_documents(query_text, documents, limit);
    std::string text;
    if (loaded.empty()) {
        text = "No matching tools found.";
    } else {
        text = std::format("Loaded {} tool{}. They are available from your next call:",
                loaded.size(),
                loaded.size() == 1 ? "" : "s");
        for (const auto& name : loaded) {
            const auto candidate = std::ranges::find(candidates, name, &ai::Tool::name);
            text += std::format("\n- {}: {}",
                    name,
                    candidate == candidates.end() ? std::string{} : first_description_line(candidate->description));
        }
    }
    Json::array_t names;
    for (const auto& name : loaded)
        names.emplace_back(name);
    return ai::ToolResultMessage{.tool_call_id = std::move(context.call_id),
            .tool_name = std::string{kToolSearchName},
            .content = {ai::text_content(std::move(text))},
            .details = Json{Json::object_t{{"loaded", Json{std::move(names)}}}}};
}

} // namespace

// debt: duplicate of upstream pi-durable search.ts BM25 ranker; consolidate when codemode/durable search unify.
Extension tool_search_extension() {
    ToolRegistration search;
    search.declaration = ai::Tool{.name = std::string{kToolSearchName},
            .description =
                    "# Tool discovery\n\nSearches over deferred tool metadata with BM25 and exposes matching tools "
                    "for the next model call.\n\nSome of the tools, such as tools of MCP servers, may not have been "
                    "provided to you upfront, and you should use this tool (`tool_search`) to search for the required "
                    "tools. For MCP tool discovery, always use `tool_search`.",
            .parameters = Json{Json::object_t{{"type", "object"},
                    {"required", Json::array_t{"query"}},
                    {"properties",
                            Json::object_t{{"query",
                                                   Json::object_t{{"type", "string"},
                                                           {"description", "Search query for deferred tools."}}},
                                    {"limit",
                                            Json::object_t{{"type", "number"},
                                                    {"description",
                                                            "Maximum number of tools to return. Defaults to 8."}}}}}}}};
    search.execute_durable = [](Json arguments, ToolExecutionContext context) {
        return execute(std::move(arguments), std::move(context));
    };
    search.replay_safe = true;
    Extension extension{.name = "tool-search"};
    extension.tools.push_back(std::move(search));
    return extension;
}

} // namespace cch::durable
