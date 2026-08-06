#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {

/// Result of a fuzzy subsequence match. Lower scores are better matches
/// (pi `fuzzyMatch`).
struct FuzzyMatch {
    bool matches{false};
    double score{0.0};
};

/// Match `query` against `text` as an ordered subsequence with
/// consecutive-match and word-boundary bonuses, an exact-match bonus, and an
/// alphanumeric-order swap fallback (pi `fuzzyMatch`).
[[nodiscard]] FuzzyMatch fuzzy_match(std::string_view query, std::string_view text);

/// Report the byte offsets into `text` of the characters matched by
/// `fuzzy_match`, or `std::nullopt` when the query does not match. An empty
/// query matches with no indices. Case folding is applied per codepoint, so
/// decomposed input (e.g. `e` + combining accent) is not composed before
/// matching.
[[nodiscard]] std::optional<std::vector<std::size_t>> fuzzy_match_indices(
    std::string_view query,
    std::string_view text);

/// Filter and rank `items` by fuzzy match quality, best first. The query is
/// split on whitespace and `/`; every token must match the item text, and the
/// summed scores determine the order (pi `fuzzyFilter`). An empty query
/// returns the items unchanged.
template <typename T, typename GetText>
[[nodiscard]] std::vector<T> fuzzy_filter(std::vector<T> items, std::string_view query, GetText get_text) {
    const auto tokens = [&]() {
        std::vector<std::string> result;
        std::size_t begin = 0;
        for (std::size_t index = 0; index <= query.size(); ++index) {
            const auto separator = index == query.size() || query[index] == '/' ||
                std::isspace(static_cast<unsigned char>(query[index])) != 0;
            if (!separator) continue;
            if (index > begin) result.emplace_back(query.substr(begin, index - begin));
            begin = index + 1;
        }
        return result;
    }();
    if (tokens.empty()) return items;

    std::vector<std::pair<double, std::size_t>> ranked;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto text = get_text(items[index]);
        double total_score = 0.0;
        bool all_match = true;
        for (const auto& token : tokens) {
            const auto match = fuzzy_match(token, text);
            if (match.matches) {
                total_score += match.score;
            } else {
                all_match = false;
                break;
            }
        }
        if (all_match) ranked.emplace_back(total_score, index);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::vector<T> result;
    result.reserve(ranked.size());
    for (const auto& [score, index] : ranked) {
        (void)score;
        result.push_back(std::move(items[index]));
    }
    return result;
}

} // namespace cch::tui
