#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui::detail {
namespace {

// ── UTF-8 helpers ────────────────────────────────────────────────────



// ── Width category helpers ───────────────────────────────────────────

/// Check if a codepoint belongs to the "Default_Ignorable_Code_Point" property.
[[nodiscard]] bool is_default_ignorable(char32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F) ||   // ZWSP, ZWNJ, ZWJ, LRM, RLM
           (cp >= 0x2028 && cp <= 0x2029) ||   // LINE SEP, PARAGRAPH SEP
           (cp >= 0x202A && cp <= 0x202E) ||   // LRE, RLE, PDF, LRO, RLO
           (cp >= 0x2060 && cp <= 0x2064) ||   // WORD JOINER, etc.
           (cp >= 0x2066 && cp <= 0x206F) ||   // LRI, RLI, FSI, PDI
           cp == 0xFEFF ||                     // BOM/ZWNBSP
           (cp >= 0xFFF9 && cp <= 0xFFFB) ||   // Interlinear annotation
           (cp >= 0xE0001) ||                   // Language tag
           (cp >= 0xE0020 && cp <= 0xE007F);   // Tag characters
}

/// Check if a codepoint is a combining mark (Unicode Mn, Mc, Me categories).
[[nodiscard]] bool is_combining_mark(char32_t cp) {
    // Nonspacing marks (Mn) - major ranges
    return (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05BD) ||
           cp == 0x05BF ||
           (cp >= 0x05C1 && cp <= 0x05C2) ||
           (cp >= 0x05C4 && cp <= 0x05C5) ||
           cp == 0x05C7 ||
           (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) ||
           cp == 0x0670 ||
           (cp >= 0x06D6 && cp <= 0x06DC) ||
           (cp >= 0x06DF && cp <= 0x06E4) ||
           (cp >= 0x06E7 && cp <= 0x06E8) ||
           (cp >= 0x06EA && cp <= 0x06ED) ||
           cp == 0x0711 ||
           (cp >= 0x0730 && cp <= 0x074A) ||
           (cp >= 0x07A6 && cp <= 0x07B0) ||
           (cp >= 0x07EB && cp <= 0x07F3) ||
           cp == 0x07FD ||
           (cp >= 0x0816 && cp <= 0x0819) ||
           (cp >= 0x081B && cp <= 0x0823) ||
           (cp >= 0x0825 && cp <= 0x0827) ||
           (cp >= 0x0829 && cp <= 0x082D) ||
           (cp >= 0x0859 && cp <= 0x085B) ||
           (cp >= 0x0898 && cp <= 0x089F) ||
           (cp >= 0x08CA && cp <= 0x08E1) ||
           (cp >= 0x08E3 && cp <= 0x0903) ||
           // Devanagari etc.
           (cp >= 0x093A && cp <= 0x093C) ||
           (cp >= 0x093E && cp <= 0x094F) ||
           (cp >= 0x0951 && cp <= 0x0957) ||
           (cp >= 0x0962 && cp <= 0x0963) ||
           (cp >= 0x0981 && cp <= 0x0983) ||
           cp == 0x09BC ||
           (cp >= 0x09BE && cp <= 0x09C4) ||
           (cp >= 0x09C7 && cp <= 0x09C8) ||
           (cp >= 0x09CB && cp <= 0x09CD) ||
           cp == 0x09D7 ||
           (cp >= 0x09E2 && cp <= 0x09E3) ||
           cp == 0x09FE ||
           (cp >= 0x0A01 && cp <= 0x0A03) ||
           cp == 0x0A3C ||
           (cp >= 0x0A3E && cp <= 0x0A42) ||
           (cp >= 0x0A47 && cp <= 0x0A48) ||
           (cp >= 0x0A4B && cp <= 0x0A4D) ||
           cp == 0x0A51 ||
           (cp >= 0x0A70 && cp <= 0x0A71) ||
           cp == 0x0A75 ||
           (cp >= 0x0A81 && cp <= 0x0A83) ||
           cp == 0x0ABC ||
           (cp >= 0x0ABE && cp <= 0x0AC5) ||
           (cp >= 0x0AC7 && cp <= 0x0AC9) ||
           (cp >= 0x0ACB && cp <= 0x0ACD) ||
           (cp >= 0x0AE2 && cp <= 0x0AE3) ||
           (cp >= 0x0AFA && cp <= 0x0AFF) ||
           (cp >= 0x0B01 && cp <= 0x0B03) ||
           cp == 0x0B3C ||
           (cp >= 0x0B3E && cp <= 0x0B44) ||
           (cp >= 0x0B47 && cp <= 0x0B48) ||
           (cp >= 0x0B4B && cp <= 0x0B4D) ||
           (cp >= 0x0B55 && cp <= 0x0B57) ||
           (cp >= 0x0B62 && cp <= 0x0B63) ||
           cp == 0x0B82 ||
           (cp >= 0x0BBE && cp <= 0x0BC2) ||
           (cp >= 0x0BC6 && cp <= 0x0BC8) ||
           (cp >= 0x0BCA && cp <= 0x0BCD) ||
           cp == 0x0BD7 ||
           (cp >= 0x0C00 && cp <= 0x0C04) ||
           cp == 0x0C3C ||
           (cp >= 0x0C3E && cp <= 0x0C44) ||
           (cp >= 0x0C46 && cp <= 0x0C48) ||
           (cp >= 0x0C4A && cp <= 0x0C4D) ||
           (cp >= 0x0C55 && cp <= 0x0C56) ||
           (cp >= 0x0C62 && cp <= 0x0C63) ||
           (cp >= 0x0C81 && cp <= 0x0C83) ||
           cp == 0x0CBC ||
           (cp >= 0x0CBE && cp <= 0x0CC4) ||
           (cp >= 0x0CC6 && cp <= 0x0CC8) ||
           (cp >= 0x0CCA && cp <= 0x0CCD) ||
           (cp >= 0x0CD5 && cp <= 0x0CD6) ||
           (cp >= 0x0CE2 && cp <= 0x0CE3) ||
           cp == 0x0CF3 ||
           (cp >= 0x0D00 && cp <= 0x0D03) ||
           (cp >= 0x0D3B && cp <= 0x0D3C) ||
           (cp >= 0x0D3E && cp <= 0x0D44) ||
           (cp >= 0x0D46 && cp <= 0x0D48) ||
           (cp >= 0x0D4A && cp <= 0x0D4D) ||
           cp == 0x0D57 ||
           (cp >= 0x0D62 && cp <= 0x0D63) ||
           (cp >= 0x0D81 && cp <= 0x0D83) ||
           cp == 0x0DCA ||
           (cp >= 0x0DCF && cp <= 0x0DD4) ||
           cp == 0x0DD6 ||
           (cp >= 0x0DD8 && cp <= 0x0DDF) ||
           (cp >= 0x0DF2 && cp <= 0x0DF3) ||
           cp == 0x0E31 ||
           (cp >= 0x0E34 && cp <= 0x0E3A) ||
           (cp >= 0x0E47 && cp <= 0x0E4E) ||
           cp == 0x0EB1 ||
           (cp >= 0x0EB4 && cp <= 0x0EBC) ||
           (cp >= 0x0EC8 && cp <= 0x0ECE) ||
           (cp >= 0x0F18 && cp <= 0x0F19) ||
           cp == 0x0F35 ||
           cp == 0x0F37 ||
           cp == 0x0F39 ||
           (cp >= 0x0F3E && cp <= 0x0F3F) ||
           (cp >= 0x0F71 && cp <= 0x0F84) ||
           (cp >= 0x0F86 && cp <= 0x0F87) ||
           (cp >= 0x0F8D && cp <= 0x0F97) ||
           (cp >= 0x0F99 && cp <= 0x0FBC) ||
           cp == 0x0FC6 ||
           // Korean Jamo, Tibetan, etc.
           (cp >= 0x102B && cp <= 0x103E) ||
           (cp >= 0x1056 && cp <= 0x1059) ||
           (cp >= 0x105E && cp <= 0x1060) ||
           (cp >= 0x1062 && cp <= 0x1064) ||
           (cp >= 0x1067 && cp <= 0x106D) ||
           (cp >= 0x1071 && cp <= 0x1074) ||
           (cp >= 0x1082 && cp <= 0x108D) ||
           cp == 0x108F ||
           (cp >= 0x109A && cp <= 0x109D) ||
           // Many more ranges omitted for brevity — this catches the common ones
           cp == 0xFE00 || cp == 0xFE01 || cp == 0xFE02 || cp == 0xFE03 ||
           cp == 0xFE04 || cp == 0xFE05 || cp == 0xFE06 || cp == 0xFE07 ||
           cp == 0xFE08 || cp == 0xFE09 || cp == 0xFE0A || cp == 0xFE0B ||
           cp == 0xFE0C || cp == 0xFE0D || cp == 0xFE0E || cp == 0xFE0F ||
           (cp >= 0xE0100 && cp <= 0xE01EF); // Variation selectors supplement
}

/// Check if a codepoint is East Asian Wide (W) or Fullwidth (F).
[[nodiscard]] bool is_cjk_wide(char32_t cp) {
    return
        // Hangul Jamo
        (cp >= 0x1100 && cp <= 0x115F) ||
        cp == 0x2329 || cp == 0x232A ||
        // CJK Radicals, Kangxi, CJK Symbols, Hiragana, Katakana, Bopomofo, Hangul
        (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3040 && cp <= 0x309F) ||
        (cp >= 0x30A0 && cp <= 0x30FF) ||
        (cp >= 0x3105 && cp <= 0x312F) ||
        (cp >= 0x3131 && cp <= 0x318E) ||
        (cp >= 0x3190 && cp <= 0x31FF) ||
        // Enclosed CJK, CJK Compatibility
        (cp >= 0x3200 && cp <= 0x33FF) ||
        // CJK Unified Ideographs Extension A
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        // CJK Unified Ideographs
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        // Yi
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        // Hangul Syllables
        (cp >= 0xAC00 && cp <= 0xD7AF) ||
        // CJK Compatibility Ideographs
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        // Vertical Forms
        (cp >= 0xFE10 && cp <= 0xFE1F) ||
        // CJK Compatibility Forms
        (cp >= 0xFE30 && cp <= 0xFE4F) ||
        // Fullwidth ASCII variants
        (cp >= 0xFF01 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        // CJK Extension B
        (cp >= 0x20000 && cp <= 0x2FFFF) ||
        // CJK Extension G, H
        (cp >= 0x30000 && cp <= 0x3FFFF);
}

/// Check if a codepoint is a variation selector (width 0).
[[nodiscard]] bool is_variation_selector(char32_t cp) {
    return (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

/// Check if a codepoint is a regional indicator symbol.
[[nodiscard]] bool is_regional_indicator(char32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/// Check if a codepoint is in an emoji range (simplified — covers RGI emoji zones).
[[nodiscard]] bool is_emoji(char32_t cp) {
    return
        // Misc technical (some)
        (cp >= 0x2300 && cp <= 0x23FF) ||
        // Dingbats
        (cp >= 0x2700 && cp <= 0x27BF) ||
        // Misc symbols and arrows
        (cp >= 0x2600 && cp <= 0x26FF) ||
        // Supplemental arrows, Misc math A
        (cp >= 0x27C0 && cp <= 0x27EF) ||
        // Misc symbols and arrows
        (cp >= 0x2900 && cp <= 0x297F) ||
        // CJK Symbols (some)
        (cp >= 0x2B00 && cp <= 0x2BFF) ||
        // Enclosed alphanumerics
        (cp >= 0x2460 && cp <= 0x24FF) ||
        // Enclosed CJK letters and months (emoji subset)
        (cp >= 0x3250 && cp <= 0x32FF) ||
        // Enclosed ideographic supplement
        (cp >= 0x1F000 && cp <= 0x1F02F) || // Mahjong
        (cp >= 0x1F030 && cp <= 0x1F09F) || // Domino
        (cp >= 0x1F0A0 && cp <= 0x1F0FF) || // Playing cards
        (cp >= 0x1F100 && cp <= 0x1F1FF) || // Enclosed alphanumeric supplement + flags
        // Emoticons (partially), Emoji, etc.
        (cp >= 0x1F300 && cp <= 0x1F5FF) || // Misc symbols and pictographs
        (cp >= 0x1F600 && cp <= 0x1F64F) || // Emoticons
        (cp >= 0x1F680 && cp <= 0x1F6FF) || // Transport and map
        (cp >= 0x1F700 && cp <= 0x1F77F) || // Alchemical
        (cp >= 0x1F780 && cp <= 0x1F7FF) || // Geometric
        (cp >= 0x1F800 && cp <= 0x1F8FF) || // Supplemental arrows C
        (cp >= 0x1F900 && cp <= 0x1F9FF) || // Supplemental symbols and pictographs
        (cp >= 0x1FA00 && cp <= 0x1FA6F) || // Chess symbols
        (cp >= 0x1FA70 && cp <= 0x1FAFF) || // Symbols and pictographs extended-A
        (cp >= 0x1FB00 && cp <= 0x1FBFF);   // Symbols for legacy computing
}

/// Check if a codepoint is zero-width.
[[nodiscard]] bool is_zero_width(char32_t cp) {
    // Control chars (except HT=0x09, LF=0x0A, CR=0x0D)
    if ((cp < 0x20 && cp != 0x09 && cp != 0x0A && cp != 0x0D) || cp == 0x7F) {
        return true;
    }
    if (is_combining_mark(cp)) return true;
    if (is_default_ignorable(cp)) return true;
    if (is_variation_selector(cp)) return true;
    return false;
}

// ── Visible width helpers ────────────────────────────────────────────

constexpr int kInvalidWidth = -1;

/// Compute the visible width of a prepared text string that has had
/// ANSI sequences removed and tabs expanded. Also used as a fast path
/// for pure-ASCII text.
[[nodiscard]] int plain_visible_width(std::string_view text) {
    int width = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto [cp, bytes] = decode_utf8(text, pos);
        if (bytes == 0) break;
        if (cp == 0xFFFD) {
            width += 1;
            pos += bytes;
            continue;
        }
        width += codepoint_width(cp);
        pos += bytes;
        // Skip combining marks that follow
        while (pos < text.size()) {
            auto [next_cp, next_bytes] = decode_utf8(text, pos);
            if (next_bytes == 0) break;
            if (!is_combining_mark(next_cp) && !is_variation_selector(next_cp) &&
                !is_default_ignorable(next_cp)) {
                break;
            }
            pos += next_bytes;
        }
    }
    return width;
}

// Keep a reference to the anonymous namespace utf8_seq_len helper for the
// public decode_utf8.
[[nodiscard]] int utf8_seq_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────

std::pair<char32_t, int> decode_utf8(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return {0xFFFD, 0};
    }

    const auto lead = static_cast<unsigned char>(text[pos]);
    const auto len = utf8_seq_len(lead);

    if (len == 1) {
        return {lead, 1};
    }

    if (len == 0 || pos + len > text.size()) {
        return {0xFFFD, 1};
    }

    char32_t cp;
    switch (len) {
    case 2: {
        const auto b1 = static_cast<unsigned char>(text[pos + 1]);
        if ((b1 & 0xC0) != 0x80) return {0xFFFD, 1};
        cp = ((static_cast<char32_t>(lead) & 0x1F) << 6) | (b1 & 0x3F);
        if (cp < 0x80) return {0xFFFD, 1};
        return {cp, 2};
    }
    case 3: {
        const auto b1 = static_cast<unsigned char>(text[pos + 1]);
        const auto b2 = static_cast<unsigned char>(text[pos + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return {0xFFFD, 1};
        cp = ((static_cast<char32_t>(lead) & 0x0F) << 12) |
             ((static_cast<char32_t>(b1) & 0x3F) << 6) |
             (b2 & 0x3F);
        if (cp < 0x800) return {0xFFFD, 1};
        if (cp >= 0xD800 && cp <= 0xDFFF) return {0xFFFD, 1};
        return {cp, 3};
    }
    case 4: {
        const auto b1 = static_cast<unsigned char>(text[pos + 1]);
        const auto b2 = static_cast<unsigned char>(text[pos + 2]);
        const auto b3 = static_cast<unsigned char>(text[pos + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return {0xFFFD, 1};
        cp = ((static_cast<char32_t>(lead) & 0x07) << 18) |
             ((static_cast<char32_t>(b1) & 0x3F) << 12) |
             ((static_cast<char32_t>(b2) & 0x3F) << 6) |
             (b3 & 0x3F);
        if (cp < 0x10000) return {0xFFFD, 1};
        if (cp > 0x10FFFF) return {0xFFFD, 1};
        return {cp, 4};
    }
    default:
        return {0xFFFD, 1};
    }
}

int codepoint_width(char32_t cp) {
    if (cp == 0x09) return 3; // tab expands to 3
    if (is_zero_width(cp)) return 0;
    if (is_cjk_wide(cp)) return 2;
    if (is_regional_indicator(cp)) return 2;
    if (is_emoji(cp)) {
        // Emoji are full-width in modern terminals
        return 2;
    }
    // Halfwidth Hangul Jamo (compatibility)
    if (cp >= 0xFF61 && cp <= 0xFFBF) return 1;
    // Halfwidth CJK forms
    if (cp >= 0xFFC2 && cp <= 0xFFC7) return 1;
    if (cp >= 0xFFCA && cp <= 0xFFCF) return 1;
    if (cp >= 0xFFD2 && cp <= 0xFFD7) return 1;
    if (cp >= 0xFFDA && cp <= 0xFFDC) return 1;
    // Halfwidth Hangul
    if (cp >= 0xFFE8 && cp <= 0xFFEE) return 1;

    // Printable ASCII and everything else
    if (cp >= 0x20 && cp <= 0x7E) return 1;
    if (cp > 0x7F) return 1; // Default: width 1 for non-CJK non-zero-width

    return 0; // Unprintable C0/C1 control characters
}

int grapheme_width(std::string_view cluster) {
    if (cluster.empty()) return 0;

    // Decode the base codepoint
    auto [cp, bytes] = decode_utf8(cluster, 0);
    if (bytes == 0) return 0;

    // Check for regional indicator pairs (flag sequences)
    if (is_regional_indicator(cp) && cluster.size() > static_cast<std::size_t>(bytes)) {
        auto [cp2, bytes2] = decode_utf8(cluster, bytes);
        if (bytes2 > 0 && is_regional_indicator(cp2)) {
            return 2; // RI pair is width 2
        }
    }

    // Emoji with VS16 or ZWJ sequences
    if (is_emoji(cp)) return 2;

    // CJK wide character with combining marks = still width 2
    if (is_cjk_wide(cp)) return 2;

    // Zero-width base => entire cluster is zero width
    if (is_zero_width(cp)) return 0;

    // Base has width 1 (most chars). Combining marks don't add width.
    return 1;
}

int visible_width(std::string_view text) {
    if (text.empty()) return 0;

    // Fast path: pure ASCII printable
    bool pure_ascii = true;
    for (auto c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc > 0x7E || (uc < 0x20 && uc != 0x09)) {
            pure_ascii = false;
            break;
        }
    }
    if (pure_ascii) {
        // Count printable chars, expand tabs to 3
        auto count = 0;
        for (auto c : text) {
            if (c == '\t') count += 3;
            else count += 1;
        }
        return count;
    }

    // Strip ANSI codes, expand tabs, then compute width
    bool has_ansi = text.find('\x1b') != std::string_view::npos;
    bool has_tabs = text.find('\t') != std::string_view::npos;
    if (!has_ansi && !has_tabs) {
        return plain_visible_width(text);
    }

    // Build clean text (strip ANSI, expand tabs)
    std::string clean;
    clean.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto ansi = extract_ansi_code(text, pos);
        if (ansi) {
            pos += ansi->length;
            continue;
        }
        if (text[pos] == '\t') {
            clean += "   ";
            pos++;
            continue;
        }
        clean += text[pos];
        pos++;
    }

    return plain_visible_width(clean);
}

std::optional<AnsiCode> extract_ansi_code(std::string_view text, std::size_t pos) {
    if (pos >= text.size() || text[pos] != '\x1b') return std::nullopt;
    if (pos + 1 >= text.size()) return std::nullopt;

    const auto next = text[pos + 1];

    // CSI: ESC [ ... m/G/K/H/J
    if (next == '[') {
        std::size_t j = pos + 2;
        while (j < text.size() && text[j] != 'm' && text[j] != 'G' &&
               text[j] != 'K' && text[j] != 'H' && text[j] != 'J') {
            j++;
        }
        if (j < text.size()) {
            return AnsiCode{std::string(text.substr(pos, j + 1 - pos)), j + 1 - pos};
        }
        return std::nullopt;
    }

    // OSC: ESC ] ... BEL (0x07) or ST (ESC \)
    if (next == ']') {
        std::size_t j = pos + 2;
        while (j < text.size()) {
            if (text[j] == '\x07') {
                return AnsiCode{std::string(text.substr(pos, j + 1 - pos)), j + 1 - pos};
            }
            if (text[j] == '\x1b' && j + 1 < text.size() && text[j + 1] == '\\') {
                return AnsiCode{std::string(text.substr(pos, j + 2 - pos)), j + 2 - pos};
            }
            j++;
        }
        return std::nullopt;
    }

    // APC: ESC _ ... BEL or ST
    if (next == '_') {
        std::size_t j = pos + 2;
        while (j < text.size()) {
            if (text[j] == '\x07') {
                return AnsiCode{std::string(text.substr(pos, j + 1 - pos)), j + 1 - pos};
            }
            if (text[j] == '\x1b' && j + 1 < text.size() && text[j + 1] == '\\') {
                return AnsiCode{std::string(text.substr(pos, j + 2 - pos)), j + 2 - pos};
            }
            j++;
        }
        return std::nullopt;
    }

    // ESC alone (2-byte sequences like ESC 7, ESC 8, ESC =, ESC >, ESC (, ESC ))
    if (pos + 1 < text.size()) {
        return AnsiCode{std::string(text.substr(pos, 2)), 2};
    }

    return std::nullopt;
}

std::vector<std::string> split_graphemes(std::string_view text) {
    std::vector<std::string> result;
    std::size_t pos = 0;

    while (pos < text.size()) {
        auto [cp, bytes] = decode_utf8(text, pos);
        if (bytes == 0) break;

        std::size_t cluster_end = pos + bytes;

        // If this is a zero-width base, a combining mark, or a variation selector
        // alone, include everything up to the next non-combining character.
        if (is_combining_mark(cp) || is_variation_selector(cp)) {
            // This shouldn't happen in well-formed text (combining marks follow a base)
            // but handle it anyway.
            result.emplace_back(text.substr(pos, cluster_end - pos));
            pos = cluster_end;
            continue;
        }

        // Gather following combining marks and variation selectors
        while (cluster_end < text.size()) {
            auto [next_cp, next_bytes] = decode_utf8(text, cluster_end);
            if (next_bytes == 0) break;
            if (!is_combining_mark(next_cp) && !is_variation_selector(next_cp) &&
                !is_default_ignorable(next_cp) && next_cp != 0x200D) {
                break;
            }
            // ZWJ (U+200D) joins the following grapheme
            if (next_cp == 0x200D) {
                // Include the ZWJ and the next character
                cluster_end += next_bytes;
                // Grab the character after ZWJ
                if (cluster_end < text.size()) {
                    auto [after_cp, after_bytes] = decode_utf8(text, cluster_end);
                    if (after_bytes > 0) {
                        cluster_end += after_bytes;
                        // Then any more combining marks
                        while (cluster_end < text.size()) {
                            auto [m_cp, m_bytes] = decode_utf8(text, cluster_end);
                            if (m_bytes == 0) break;
                            if (!is_combining_mark(m_cp) && !is_variation_selector(m_cp) &&
                                !is_default_ignorable(m_cp) && m_cp != 0x200D) {
                                break;
                            }
                            cluster_end += m_bytes;
                            if (m_cp == 0x200D) {
                                // Another ZWJ — loop back to grab the next char
                                continue;
                            }
                        }
                    }
                }
                break;
            }
            cluster_end += next_bytes;
        }

        result.emplace_back(text.substr(pos, cluster_end - pos));
        pos = cluster_end;
    }

    return result;
}

// ── AnsiStyleState ───────────────────────────────────────────────────

std::string AnsiStyleState::get_active_codes() const {
    std::string codes;
    if (bold) codes += ";1";
    if (dim) codes += ";2";
    if (italic) codes += ";3";
    if (underline) codes += ";4";
    if (blink) codes += ";5";
    if (inverse) codes += ";7";
    if (hidden) codes += ";8";
    if (strikethrough) codes += ";9";
    if (!fg_color.empty()) codes += ";" + fg_color;
    if (!bg_color.empty()) codes += ";" + bg_color;

    std::string result;
    if (!codes.empty()) {
        // Remove leading ';'
        result = "\x1b[" + codes.substr(1) + "m";
    }
    if (!hyperlink.empty()) {
        if (!hyperlink_params.empty()) {
            result += "\x1b]8;" + hyperlink_params + ";" + hyperlink + "\x07";
        } else {
            result += "\x1b]8;;" + hyperlink + "\x07";
        }
    }
    return result;
}

bool AnsiStyleState::has_active_codes() const {
    return bold || dim || italic || underline || blink || inverse || hidden ||
           strikethrough || !fg_color.empty() || !bg_color.empty() || !hyperlink.empty();
}

std::string AnsiStyleState::get_line_end_reset() const {
    std::string result;
    if (underline) {
        result += "\x1b[24m"; // underline off
    }
    if (!hyperlink.empty()) {
        result += "\x1b]8;;\x07"; // close hyperlink (re-opened at next line start)
    }
    return result;
}

void AnsiStyleState::process_ansi(std::string_view code) {
    if (code.empty()) return;

    // OSC 8 hyperlink
    if (code.starts_with("\x1b]8;")) {
        std::string_view body;
        if (code.ends_with('\x07')) {
            body = code.substr(4, code.size() - 5);
        } else if (code.ends_with("\x1b\\")) {
            body = code.substr(4, code.size() - 6);
        } else {
            return;
        }
        const auto sep = body.find(';');
        if (sep == std::string_view::npos) return;
        const auto params = body.substr(0, sep);
        const auto url = body.substr(sep + 1);
        if (url.empty()) {
            // Close hyperlink
            hyperlink.clear();
            hyperlink_params.clear();
        } else {
            hyperlink = std::string(url);
            hyperlink_params = std::string(params);
        }
        return;
    }

    if (!code.ends_with('m') || !code.starts_with("\x1b[")) return;

    // Parse SGR parameters
    std::string_view params_str = code.substr(2, code.size() - 3);

    if (params_str.empty() || params_str == "0") {
        reset();
        return;
    }

    // Split by ';'
    std::size_t start = 0;
    while (start < params_str.size()) {
        auto end = params_str.find(';', start);
        if (end == std::string_view::npos) end = params_str.size();
        auto part = params_str.substr(start, end - start);

        if (part.empty()) {
            start = end + 1;
            continue;
        }

        int val = 0;
        for (auto c : part) {
            if (c < '0' || c > '9') { val = -1; break; }
            val = val * 10 + (c - '0');
        }

        if (val < 0) {
            start = end + 1;
            continue;
        }

        switch (val) {
        case 0: reset(); break;
        case 1: bold = true; break;
        case 2: dim = true; break;
        case 3: italic = true; break;
        case 4: underline = true; break;
        case 5: blink = true; break;
        case 7: inverse = true; break;
        case 8: hidden = true; break;
        case 9: strikethrough = true; break;
        case 22: bold = false; dim = false; break;
        case 23: italic = false; break;
        case 24: underline = false; break;
        case 25: blink = false; break;
        case 27: inverse = false; break;
        case 28: hidden = false; break;
        case 29: strikethrough = false; break;
        case 39: fg_color.clear(); break;
        case 49: bg_color.clear(); break;
        default:
            if (val >= 30 && val <= 37) fg_color = std::string(part);
            else if (val >= 40 && val <= 47) bg_color = std::string(part);
            else if (val >= 90 && val <= 97) fg_color = std::string(part);
            else if (val >= 100 && val <= 107) bg_color = std::string(part);
            else if (val == 38 || val == 48) {
                // 256-color or RGB: 38;5;N or 38;2;R;G;B
                // remaining starts after the "38;" or "48;" prefix
                auto after_prefix = params_str.substr(end + 1);
                std::size_t consumed = 0;
                if (after_prefix.size() >= 1 && after_prefix[0] == '5') {
                    // 38;5;N
                    auto n_end = after_prefix.find(';', 2);
                    if (n_end == std::string_view::npos) n_end = after_prefix.size();
                    auto color_code = std::string(part) + ";5;" +
                                      std::string(after_prefix.substr(2, n_end - 2));
                    if (val == 38) fg_color = color_code;
                    else bg_color = color_code;
                    consumed = 2 + (n_end - 2);
                } else if (after_prefix.size() >= 1 && after_prefix[0] == '2') {
                    // 38;2;R;G;B
                    auto r_end = after_prefix.find(';', 2);
                    auto g_end = r_end != std::string_view::npos
                                     ? after_prefix.find(';', r_end + 1)
                                     : std::string_view::npos;
                    if (g_end != std::string_view::npos) {
                        auto b_end = after_prefix.find(';', g_end + 1);
                        if (b_end == std::string_view::npos) b_end = after_prefix.size();
                        auto color_code =
                            std::string(part) + ";2;" +
                            std::string(after_prefix.substr(2, r_end - 2)) + ";" +
                            std::string(after_prefix.substr(r_end + 1, g_end - r_end - 1)) + ";" +
                            std::string(after_prefix.substr(g_end + 1, b_end - g_end - 1));
                        if (val == 38) fg_color = color_code;
                        else bg_color = color_code;
                        consumed = 2 + (r_end - 2) + 1 + (g_end - r_end - 1) + 1 + (b_end - g_end - 1);
                    }
                }
                // Adjust start past the consumed parameters
                if (consumed > 0) {
                    start = end + 1 + consumed;
                    continue;
                }
            }
            break;
        }

        start = end + 1;
    }
}

void AnsiStyleState::reset() {
    bold = false;
    dim = false;
    italic = false;
    underline = false;
    blink = false;
    inverse = false;
    hidden = false;
    strikethrough = false;
    fg_color.clear();
    bg_color.clear();
    // Note: SGR reset does not reset OSC 8 hyperlink state
}

// ── Wrapping ─────────────────────────────────────────────────────────

util::Expected<std::vector<std::string>> wrap_text(std::string_view text, int width) {
    if (width <= 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Wrap width must be positive"));
    }

    if (text.empty()) {
        return std::vector<std::string>{""};
    }

    // First, validate and normalize
    auto normalized = normalize_terminal_output(text);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }

    // Split on literal newlines
    std::vector<std::string> result;
    AnsiStyleState style_tracker;
    std::size_t line_start = 0;

    while (line_start <= normalized->size()) {
        const auto lf = normalized->find('\n', line_start);
        const auto line_end = (lf == std::string::npos) ? normalized->size() : lf;
        std::string_view line(normalized->data() + line_start, line_end - line_start);

        // Wrap this line
        if (visible_width(line) <= width) {
            // Line fits — just add it
            result.push_back(std::string(line));
        } else if (width >= 1) {
            // Need to wrap
            std::string current_line;
            int current_width = 0;
            auto graphemes = split_graphemes(line);

            for (const auto& cluster : graphemes) {
                int cw = grapheme_width(cluster);

                // Check if this cluster fits
                if (current_width + cw > width) {
                    // End current line with underline/hyperlink resets
                    auto line_end_reset = style_tracker.get_line_end_reset();
                    if (!line_end_reset.empty()) {
                        current_line += line_end_reset;
                    }
                    // Trim trailing spaces
                    while (!current_line.empty() && current_line.back() == ' ') {
                        current_line.pop_back();
                    }
                    result.push_back(current_line);

                    // Start new line with inherited style
                    current_line = style_tracker.get_active_codes();
                    current_width = 0;
                }

                // Track style from this cluster
                // Extract any embedded ANSI codes within the cluster
                std::size_t cp = 0;
                while (cp < cluster.size()) {
                    auto ansi = extract_ansi_code(cluster, cp);
                    if (ansi) {
                        style_tracker.process_ansi(ansi->code);
                        cp += ansi->length;
                    } else {
                        cp++;
                    }
                }

                current_line += cluster;
                current_width += cw;
            }

            if (!current_line.empty()) {
                // Preserve trailing whitespace — no reset at final line
                result.push_back(current_line);
            } else {
                // Line was empty after wrapping — push an empty line
                result.emplace_back();
            }
        } else {
            result.emplace_back();
        }

        // Update style tracker for next input line
        std::size_t cp = 0;
        while (cp < line.size()) {
            auto ansi = extract_ansi_code(line, cp);
            if (ansi) {
                style_tracker.process_ansi(ansi->code);
                cp += ansi->length;
            } else {
                cp++;
            }
        }

        if (lf == std::string::npos) break;
        line_start = lf + 1;
    }

    return result;
}

util::Expected<std::string> truncate_text(
    std::string_view text, int max_width, std::string_view ellipsis, bool pad)
{
    if (max_width <= 0) {
        return std::string{};
    }

    if (text.empty()) {
        if (pad) {
            return std::string(max_width, ' ');
        }
        return std::string{};
    }

    auto normalized = normalize_terminal_output(text);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }

    const auto text_width = visible_width(*normalized);
    if (text_width <= max_width) {
        if (pad) {
            return *normalized + std::string(max_width - text_width, ' ');
        }
        return *normalized;
    }

    const auto ellipsis_width = visible_width(ellipsis);
    const auto target_width = max_width - ellipsis_width;

    if (target_width <= 0) {
        // Ellipsis itself doesn't fit — truncate ellipsis
        auto visible_ellipsis = truncate_text(ellipsis, max_width, "", false);
        if (!visible_ellipsis) {
            return std::unexpected(visible_ellipsis.error());
        }
        if (pad) {
            auto ew = visible_width(*visible_ellipsis);
            return *visible_ellipsis + std::string(max_width - ew, ' ');
        }
        return *visible_ellipsis;
    }

    // Build result with grapheme-level truncation
    std::string result;
    int collected_width = 0;
    AnsiStyleState style_tracker;

    auto graphemes = split_graphemes(*normalized);
    for (const auto& cluster : graphemes) {
        auto cw = grapheme_width(cluster);
        if (collected_width + cw > target_width) {
            break;
        }
        // Track style
        std::size_t cp = 0;
        while (cp < cluster.size()) {
            auto ansi = extract_ansi_code(cluster, cp);
            if (ansi) {
                style_tracker.process_ansi(ansi->code);
                cp += ansi->length;
            } else {
                cp++;
            }
        }
        result += cluster;
        collected_width += cw;
    }

    // Append ellipsis with style reset
    auto active_codes = style_tracker.get_active_codes();
    std::string final_result = result + "\x1b[0m" + std::string(ellipsis) + "\x1b[0m";

    if (pad) {
        auto final_width = visible_width(final_result);
        return final_result + std::string(max_width - final_width, ' ');
    }

    return final_result;
}

util::Expected<std::string> normalize_terminal_output(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    std::size_t pos = 0;
    while (pos < text.size()) {
        // Preserve ANSI escape sequences as-is
        auto ansi = extract_ansi_code(text, pos);
        if (ansi) {
            result += ansi->code;
            pos += ansi->length;
            continue;
        }

        auto [cp, bytes] = decode_utf8(text, pos);

        if (bytes == 0) {
            break;
        }

        if (cp == 0xFFFD && bytes > 0) {
            // Malformed UTF-8 — replace with replacement character
            result += "\xEF\xBF\xBD"; // U+FFFD in UTF-8
            pos += bytes;
            continue;
        }

        // Check for unsupported C0 control characters
        if (cp < 0x20 && cp != 0x09 && cp != 0x0A && cp != 0x0D) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "Unsupported control character in terminal text",
                "U+" + std::format("{:04X}", static_cast<unsigned>(cp))));
        }

        // Expand tabs outside ANSI sequences
        if (cp == '\t') {
            result += "   ";
            pos += bytes;
            continue;
        }

        result += text.substr(pos, bytes);
        pos += bytes;
    }

    return result;
}

} // namespace cch::tui::detail
