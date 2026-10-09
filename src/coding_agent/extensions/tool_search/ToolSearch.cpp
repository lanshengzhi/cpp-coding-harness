#include "coding_agent/extensions/tool_search/ToolSearch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cch::coding_agent::extensions {
namespace {

[[nodiscard]] std::vector<std::string> terms(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) != 0 || ch == '_')
            current.push_back(static_cast<char>(std::tolower(ch)));
        else if (!current.empty()) {
            result.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

[[nodiscard]] double field_score(const std::vector<std::string>& query, std::string_view field) {
    const auto document = terms(field);
    if (query.empty() || document.empty()) return 0.0;
    std::map<std::string, std::size_t> frequencies;
    for (const auto& term : document)
        ++frequencies[term];
    double score = 0.0;
    for (const auto& term : query) {
        const auto found = frequencies.find(term);
        if (found != frequencies.end())
            score += 1.0 + std::log1p(static_cast<double>(found->second));
        else {
            for (const auto& candidate : document) {
                if (candidate.starts_with(term) || term.starts_with(candidate)) {
                    score += 0.35;
                    break;
                }
            }
        }
    }
    return score / std::sqrt(static_cast<double>(document.size()));
}

} // namespace

std::vector<ToolSearchCandidate> rank_tool_search_candidates(
        std::vector<ToolSearchCandidate> candidates, std::string_view query, std::size_t limit) {
    const auto query_terms = terms(query);
    struct Ranked {
        ToolSearchCandidate candidate;
        double score;
        std::size_t order;
    };
    std::vector<Ranked> ranked;
    std::size_t order = 0;
    for (auto& candidate : candidates) {
        const std::size_t candidate_order = order++;
        if (candidate.active ||
                (candidate.exposure != mcp::McpExposure::Deferred && candidate.exposure != mcp::McpExposure::Codemode))
            continue;
        const double score = field_score(query_terms, candidate.name) * 2.0 +
                             field_score(query_terms, candidate.namespace_name) * 1.5 +
                             field_score(query_terms, candidate.description);
        if (!query_terms.empty() && score <= 0.0) continue;
        ranked.push_back({std::move(candidate), score, candidate_order});
    }
    std::ranges::stable_sort(ranked, [](const Ranked& left, const Ranked& right) { return left.score > right.score; });
    std::vector<ToolSearchCandidate> result;
    for (auto& item : ranked) {
        if (result.size() >= limit) break;
        result.push_back(std::move(item.candidate));
    }
    return result;
}

ai::Tool tool_search_definition() {
    ai::Tool tool;
    tool.name = "tool_search";
    tool.description = "Find deferred tools by name, namespace, or description and activate them for direct use.";
    tool.parameters = support::JsonValue::object_t{
            {"type", "object"},
            {"required", support::JsonValue::array_t{"query"}},
            {"properties",
                    support::JsonValue::object_t{
                            {"query",
                                    support::JsonValue::object_t{
                                            {"type", "string"}, {"description", "What the tool should do."}}},
                            {"limit",
                                    support::JsonValue::object_t{{"type", "number"}, {"minimum", 1}, {"maximum", 50}}},
                    }},
    };
    return tool;
}

} // namespace cch::coding_agent::extensions
