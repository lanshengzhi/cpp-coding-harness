#include <cch/tui/Fuzzy.hpp>

#include <utf8proc.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {
namespace {

/// One decoded character of the caller-supplied text: its code point, the byte
/// offset it starts at, and whether it was valid UTF-8. Malformed bytes are
/// passed through unchanged so matching stays total over any byte string.
struct SourceCharacter {
    utf8proc_int32_t codepoint{0};
    std::size_t offset{0};
    bool valid{true};
};

/// Lowercased code points, each tagged with the original UTF-8 byte offset of
/// the source character that produced it.
struct FoldedCharacters {
    std::vector<utf8proc_int32_t> codepoints;
    std::vector<std::size_t> origins;
};

constexpr utf8proc_int32_t kDottedCapitalI = 0x0130;
constexpr utf8proc_int32_t kCombiningDotAbove = 0x0307;
constexpr utf8proc_int32_t kCapitalSigma = 0x03A3;
constexpr utf8proc_int32_t kSmallFinalSigma = 0x03C2;

[[nodiscard]] std::vector<SourceCharacter> decode_characters(std::string_view text) {
    std::vector<SourceCharacter> characters;
    std::size_t position = 0;
    while (position < text.size()) {
        utf8proc_int32_t codepoint = 0;
        const auto decoded = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data()) + position,
                static_cast<utf8proc_ssize_t>(text.size() - position),
                &codepoint);
        if (decoded > 0) {
            characters.push_back({codepoint, position, true});
            position += static_cast<std::size_t>(decoded);
        } else {
            characters.push_back(
                    {static_cast<utf8proc_int32_t>(static_cast<unsigned char>(text[position])), position, false});
            ++position;
        }
    }
    return characters;
}

/// Cased is Lowercase, Uppercase or Lt, the Unicode `Cased` derived property.
[[nodiscard]] bool is_cased(const SourceCharacter& character) {
    if (!character.valid) return false;
    const auto category = utf8proc_category(character.codepoint);
    return category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LT;
}

/// Case_Ignorable is Grapheme_Extend (Mn/Me/Cf/Lm/Sk) plus the single-quote,
/// mid-letter and mid-number-let word-break characters the Final_Sigma
/// condition skips over.
[[nodiscard]] bool is_case_ignorable(const SourceCharacter& character) {
    if (!character.valid) return false;
    const auto category = utf8proc_category(character.codepoint);
    if (category == UTF8PROC_CATEGORY_MN || category == UTF8PROC_CATEGORY_ME || category == UTF8PROC_CATEGORY_CF ||
            category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_SK) {
        return true;
    }
    switch (character.codepoint) {
    case 0x0027:
    case 0x002E:
    case 0x003A:
    case 0x00B7:
    case 0x0387:
    case 0x055F:
    case 0x05F4:
    case 0x2018:
    case 0x2019:
    case 0x2024:
    case 0xFE52:
    case 0xFF07:
    case 0xFF0E:
        return true;
    default:
        return false;
    }
}

/// The nearest preceding non-case-ignorable character is cased.
[[nodiscard]] bool preceded_by_cased(const std::vector<SourceCharacter>& characters, std::size_t index) {
    for (std::size_t cursor = index; cursor > 0; --cursor) {
        const auto& previous = characters[cursor - 1];
        if (!is_case_ignorable(previous)) return is_cased(previous);
    }
    return false;
}

/// The nearest following non-case-ignorable character is cased.
[[nodiscard]] bool followed_by_cased(const std::vector<SourceCharacter>& characters, std::size_t index) {
    for (auto cursor = index + 1; cursor < characters.size(); ++cursor) {
        const auto& next = characters[cursor];
        if (!is_case_ignorable(next)) return is_cased(next);
    }
    return false;
}

/// Append one source character's lowercased form. This models the frozen
/// `String.prototype.toLowerCase`: the simple per-codepoint lowercase mapping,
/// the unconditional `İ` expansion from SpecialCasing, and the context-sensitive
/// Final_Sigma condition. Full casefolding (`ß` → `ss`) and composition are not
/// part of it and would change which queries match.
void append_lowercase(const std::vector<SourceCharacter>& characters, std::size_t index, FoldedCharacters* out) {
    const auto& character = characters[index];
    const auto append = [&](utf8proc_int32_t codepoint) {
        out->codepoints.push_back(codepoint);
        out->origins.push_back(character.offset);
    };
    if (!character.valid) {
        append(character.codepoint);
        return;
    }
    if (character.codepoint == kDottedCapitalI) {
        append(0x0069);
        append(kCombiningDotAbove);
        return;
    }
    if (character.codepoint == kCapitalSigma) {
        append(preceded_by_cased(characters, index) && !followed_by_cased(characters, index)
                        ? kSmallFinalSigma
                        : utf8proc_tolower(kCapitalSigma));
        return;
    }
    append(utf8proc_tolower(character.codepoint));
}

[[nodiscard]] FoldedCharacters lowercase_characters(std::string_view text) {
    const auto characters = decode_characters(text);
    FoldedCharacters folded;
    for (std::size_t index = 0; index < characters.size(); ++index) {
        append_lowercase(characters, index, &folded);
    }
    return folded;
}

/// The lowercased text as UTF-16 code units, each carrying the original UTF-8
/// byte offset of the character that produced it. Matching happens over these
/// units, which is what gives pi's UTF-16 index semantics.
struct FoldedText {
    std::vector<char16_t> units;
    std::vector<std::size_t> origins;
};

[[nodiscard]] FoldedText to_utf16_units(const FoldedCharacters& folded) {
    FoldedText text;
    text.units.reserve(folded.codepoints.size());
    text.origins.reserve(folded.codepoints.size());
    for (std::size_t index = 0; index < folded.codepoints.size(); ++index) {
        const auto codepoint = folded.codepoints[index];
        char16_t encoded[2]{};
        std::size_t count = 1;
        if (codepoint > 0xFFFF) {
            const auto value = static_cast<std::uint32_t>(codepoint) - 0x10000;
            encoded[0] = static_cast<char16_t>(0xD800 + (value >> 10));
            encoded[1] = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
            count = 2;
        } else {
            encoded[0] = static_cast<char16_t>(codepoint);
        }
        for (std::size_t unit = 0; unit < count; ++unit) {
            text.units.push_back(encoded[unit]);
            text.origins.push_back(folded.origins[index]);
        }
    }
    return text;
}

/// The frozen `\s` class, which contains non-ASCII whitespace the C locale's
/// `isspace` reports as ordinary text.
[[nodiscard]] bool is_frozen_whitespace(utf8proc_int32_t codepoint) {
    switch (codepoint) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x0020:
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return true;
    default:
        return codepoint >= 0x2000 && codepoint <= 0x200A;
    }
}

/// pi's `/[\s\-_./:]/` word-boundary class.
[[nodiscard]] bool is_word_boundary(char16_t previous) {
    return is_frozen_whitespace(previous) || previous == u'-' || previous == u'_' || previous == u'.' ||
           previous == u'/' || previous == u':';
}

[[nodiscard]] std::optional<double> match_folded_query(
        const std::vector<char16_t>& query, const FoldedText& text, std::vector<std::size_t>* matched_units) {
    if (query.empty()) return 0.0;
    if (query.size() > text.units.size()) return std::nullopt;
    std::size_t query_index = 0;
    std::optional<std::size_t> last_match;
    double score = 0.0;
    std::size_t consecutive = 0;
    for (std::size_t index = 0; index < text.units.size() && query_index < query.size(); ++index) {
        if (text.units[index] != query[query_index]) continue;
        const auto boundary = index == 0 || is_word_boundary(text.units[index - 1]);
        // pi's scan opens with `lastMatchIndex = -1`, so the consecutive branch
        // is taken only when the first match lands at index 0; a first match
        // elsewhere neither scores a bonus nor a gap penalty.
        const auto consecutive_run = !last_match ? index == 0 : *last_match + 1 == index;
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
        if (matched_units != nullptr) matched_units->push_back(index);
        ++query_index;
    }
    if (query_index != query.size()) return std::nullopt;
    if (query == text.units) score -= 100.0;
    return score;
}

/// pi's `^(?<letters>[a-z]+)(?<digits>[0-9]+)$` fallback over the lowercased
/// query, returning the swapped order or `std::nullopt`.
[[nodiscard]] std::optional<std::vector<char16_t>> swapped_alpha_numeric(const std::vector<char16_t>& query) {
    std::size_t split = 0;
    while (split < query.size() && query[split] >= u'a' && query[split] <= u'z')
        ++split;
    if (split == 0 || split == query.size()) return std::nullopt;
    for (std::size_t index = split; index < query.size(); ++index) {
        if (query[index] < u'0' || query[index] > u'9') return std::nullopt;
    }
    std::vector<char16_t> swapped;
    swapped.reserve(query.size());
    swapped.insert(swapped.end(), query.begin() + static_cast<std::ptrdiff_t>(split), query.end());
    swapped.insert(swapped.end(), query.begin(), query.begin() + static_cast<std::ptrdiff_t>(split));
    return swapped;
}

[[nodiscard]] std::optional<double> fuzzy_score(std::string_view query, std::string_view text) {
    const auto folded_query = to_utf16_units(lowercase_characters(query));
    const auto folded_text = to_utf16_units(lowercase_characters(text));
    if (auto primary = match_folded_query(folded_query.units, folded_text, nullptr)) {
        return primary;
    }
    const auto swapped = swapped_alpha_numeric(folded_query.units);
    if (!swapped) return std::nullopt;
    if (auto matched = match_folded_query(*swapped, folded_text, nullptr)) {
        return *matched + 5.0;
    }
    return std::nullopt;
}

[[nodiscard]] std::string encode_codepoints(const std::vector<utf8proc_int32_t>& codepoints) {
    std::string text;
    for (const auto codepoint : codepoints) {
        utf8proc_uint8_t encoded[4]{};
        const auto size = utf8proc_encode_char(codepoint, encoded);
        if (size > 0) text.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(size));
    }
    return text;
}

} // namespace

namespace detail {

std::vector<std::string> fuzzy_query_tokens(std::string_view query) {
    const auto folded = lowercase_characters(query);
    std::vector<std::string> tokens;
    std::vector<utf8proc_int32_t> current;
    const auto flush = [&]() {
        if (current.empty()) return;
        tokens.push_back(encode_codepoints(current));
        current.clear();
    };
    for (const auto codepoint : folded.codepoints) {
        if (codepoint == '/' || is_frozen_whitespace(codepoint)) {
            flush();
            continue;
        }
        current.push_back(codepoint);
    }
    flush();
    return tokens;
}

} // namespace detail

FuzzyMatch fuzzy_match(std::string_view query, std::string_view text) {
    const auto score = fuzzy_score(query, text);
    if (!score) return {};
    return FuzzyMatch{.matches = true, .score = *score};
}

std::optional<std::vector<std::size_t>> fuzzy_match_indices(
    std::string_view query,
    std::string_view text) {
    if (query.empty()) return std::vector<std::size_t>{};
    const auto folded_query = to_utf16_units(lowercase_characters(query));
    const auto folded_text = to_utf16_units(lowercase_characters(text));

    std::vector<std::size_t> units;
    if (!match_folded_query(folded_query.units, folded_text, &units)) {
        units.clear();
        const auto swapped = swapped_alpha_numeric(folded_query.units);
        if (!swapped || !match_folded_query(*swapped, folded_text, &units)) return std::nullopt;
    }
    std::vector<std::size_t> offsets;
    offsets.reserve(units.size());
    for (const auto unit : units)
        offsets.push_back(folded_text.origins[unit]);
    return offsets;
}

} // namespace cch::tui
