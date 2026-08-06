#include <cch/tui/Fuzzy.hpp>

#include <utf8proc.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {
namespace {

[[nodiscard]] std::string casefold_text(std::string_view text) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto size = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
        static_cast<utf8proc_ssize_t>(text.size()),
        &mapped,
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
    if (size < 0 || mapped == nullptr) {
        std::free(mapped);
        return std::string(text);
    }
    std::string result(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(size));
    std::free(mapped);
    return result;
}

[[nodiscard]] std::optional<double> match_fuzzy_query(
    std::string_view normalized_query,
    std::string_view normalized_text,
    std::vector<std::size_t>* matched_indices) {
    if (normalized_query.empty()) return 0.0;
    if (normalized_query.size() > normalized_text.size()) return std::nullopt;
    std::size_t query_index = 0;
    std::optional<std::size_t> last_match;
    double score = 0.0;
    std::size_t consecutive = 0;
    for (std::size_t index = 0;
         index < normalized_text.size() && query_index < normalized_query.size();
         ++index) {
        if (normalized_text[index] != normalized_query[query_index]) continue;
        // pi's boundary class is `/\s\-_./:/`; the ASCII whitespace members
        // are matched byte-wise on the folded text.
        const auto boundary = index == 0 ||
            std::string_view(" \t\n\v\f\r-_./:").find(normalized_text[index - 1]) !=
                std::string_view::npos;
        // pi's scan opens with `lastMatchIndex = -1`, so the consecutive branch
        // is taken only when the first match lands at index 0; a first match
        // elsewhere neither scores a bonus nor a gap penalty.
        const auto consecutive_run =
            !last_match ? index == 0 : *last_match + 1 == index;
        if (consecutive_run) {
            ++consecutive;
            score -= static_cast<double>(consecutive * 5);
        } else {
            consecutive = 0;
            if (last_match) score += static_cast<double>((index - *last_match - 1) * 2);
        }
        if (boundary) score -= 10.0;
        score += static_cast<double>(index) * 0.1;
        last_match = index;
        if (matched_indices != nullptr) matched_indices->push_back(index);
        ++query_index;
    }
    if (query_index != normalized_query.size()) return std::nullopt;
    if (normalized_query == normalized_text) score -= 100.0;
    return score;
}

[[nodiscard]] std::optional<std::string> swapped_alpha_numeric(std::string_view query) {
    const auto split = std::find_if(query.begin(), query.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
    });
    if (split != query.begin() && split != query.end() &&
        std::all_of(query.begin(), split, [](unsigned char value) { return std::isalpha(value) != 0; }) &&
        std::all_of(split, query.end(), [](unsigned char value) { return std::isdigit(value) != 0; })) {
        return std::string(split, query.end()) + std::string(query.begin(), split);
    }
    const auto letters = std::find_if(query.begin(), query.end(), [](unsigned char value) {
        return std::isalpha(value) != 0;
    });
    if (letters != query.begin() && letters != query.end() &&
        std::all_of(query.begin(), letters, [](unsigned char value) { return std::isdigit(value) != 0; }) &&
        std::all_of(letters, query.end(), [](unsigned char value) { return std::isalpha(value) != 0; })) {
        return std::string(letters, query.end()) + std::string(query.begin(), letters);
    }
    return std::nullopt;
}

/// Casefold `text` codepoint by codepoint, recording for every folded byte the
/// byte offset in `text` of the codepoint that produced it. Per-codepoint
/// casefolding is byte-identical to whole-string casefolding without
/// composition; malformed UTF-8 bytes pass through with identity mapping.
[[nodiscard]] std::pair<std::string, std::vector<std::size_t>> casefold_with_offsets(
    std::string_view text) {
    std::string folded;
    std::vector<std::size_t> offsets;
    std::size_t position = 0;
    while (position < text.size()) {
        utf8proc_int32_t codepoint = 0;
        const auto decoded = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(text.data()) + position,
            static_cast<utf8proc_ssize_t>(text.size() - position),
            &codepoint);
        const auto length = decoded > 0 ? static_cast<std::size_t>(decoded) : 1;
        const auto bytes = text.substr(position, length);

        if (decoded > 0) {
            utf8proc_uint8_t encoded[4];
            const auto encoded_size = utf8proc_encode_char(codepoint, encoded);
            utf8proc_uint8_t* mapped = nullptr;
            const auto mapped_size = utf8proc_map(
                encoded,
                static_cast<utf8proc_ssize_t>(encoded_size),
                &mapped,
                static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_CASEFOLD));
            if (mapped_size > 0 && mapped != nullptr) {
                for (utf8proc_ssize_t index = 0; index < mapped_size; ++index) {
                    folded += static_cast<char>(mapped[index]);
                    offsets.push_back(position);
                }
                std::free(mapped);
                position += length;
                continue;
            }
            std::free(mapped);
        }

        for (const auto byte : bytes) {
            folded += byte;
            offsets.push_back(position);
        }
        position += length;
    }
    return {std::move(folded), std::move(offsets)};
}

[[nodiscard]] std::optional<double> fuzzy_score(std::string_view query, std::string_view text) {
    const auto normalized_query = casefold_text(query);
    const auto normalized_text = casefold_text(text);
    if (auto primary = match_fuzzy_query(normalized_query, normalized_text, nullptr)) {
        return primary;
    }
    const auto swapped = swapped_alpha_numeric(normalized_query);
    if (!swapped) return std::nullopt;
    if (auto matched = match_fuzzy_query(*swapped, normalized_text, nullptr)) {
        return *matched + 5.0;
    }
    return std::nullopt;
}

} // namespace

FuzzyMatch fuzzy_match(std::string_view query, std::string_view text) {
    const auto score = fuzzy_score(query, text);
    if (!score) return {};
    return FuzzyMatch{.matches = true, .score = *score};
}

std::optional<std::vector<std::size_t>> fuzzy_match_indices(
    std::string_view query,
    std::string_view text) {
    if (query.empty()) return std::vector<std::size_t>{};
    const auto [folded_text, offsets] = casefold_with_offsets(text);
    const auto folded_query = casefold_with_offsets(query).first;

    std::vector<std::size_t> indices;
    if (match_fuzzy_query(folded_query, folded_text, &indices)) return indices;
    indices.clear();
    const auto swapped = swapped_alpha_numeric(folded_query);
    if (swapped && match_fuzzy_query(*swapped, folded_text, &indices)) return indices;
    return std::nullopt;
}

} // namespace cch::tui
