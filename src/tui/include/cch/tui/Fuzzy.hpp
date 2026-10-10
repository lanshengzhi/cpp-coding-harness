#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {

namespace detail {

/// The query tokens `fuzzy_filter` requires every item to match: pi's
/// `query.trim().split(/[\s/]+/)` with empty tokens dropped, using the frozen
/// Unicode whitespace class rather than the C locale's. An empty result leaves
/// the item order unchanged.
[[nodiscard]] std::vector<std::string> fuzzy_query_tokens(std::string_view query);

} // namespace detail

/// Result of a fuzzy subsequence match. Lower scores are better matches
/// (pi `fuzzyMatch`).
struct FuzzyMatch {
    bool matches{false};
    double score{0.0};
};

/// Match `query` against `text` as an ordered subsequence with
/// consecutive-match and word-boundary bonuses, an exact-match bonus, and an
/// alphanumeric-order swap fallback (pi `fuzzyMatch`).
///
/// Query and text are lowercased with pi's `String.prototype.toLowerCase`
/// semantics: simple per-codepoint lowercase mapping plus the unconditional
/// `İ` → `i` + `U+0307` expansion and context-sensitive final sigma. Full
/// casefolding (`ß` → `ss`) and composition are deliberately not applied.
///
/// Positions, scores and therefore ranking use pi's UTF-16 index semantics:
/// matched positions are counted in UTF-16 code units of the lowercased text, so
/// a supplementary character advances two positions.
[[nodiscard]] FuzzyMatch fuzzy_match(std::string_view query, std::string_view text);

/// Report the byte offsets into the original `text` of the characters matched
/// by `fuzzy_match`, or `std::nullopt` when the query does not match. An empty
/// query matches with no indices.
///
/// This is the retained highlight contract and is deliberately separate from
/// scoring: matched positions are found with pi's UTF-16 index semantics and
/// then reported as the original UTF-8 byte offsets of the characters that
/// produced them, so highlighting targets the original text rather than a
/// folded buffer. A single source character can supply several matched units
/// (an expanded `İ`), in which case its offset is reported for each.
[[nodiscard]] std::optional<std::vector<std::size_t>> fuzzy_match_indices(
    std::string_view query,
    std::string_view text);

/// Filter and rank `items` by fuzzy match quality, best first. The query is
/// split with `detail::fuzzy_query_tokens`; every token must match the item
/// text, and the summed scores determine the order (pi `fuzzyFilter`), whose
/// UTF-16 index semantics make the ranking independent of UTF-8 byte offsets.
/// Equal scores keep the input order. An empty query returns the items
/// unchanged.
template <typename T, typename GetText>
[[nodiscard]] std::vector<T> fuzzy_filter(std::vector<T> items, std::string_view query, GetText get_text) {
    const auto tokens = detail::fuzzy_query_tokens(query);
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
