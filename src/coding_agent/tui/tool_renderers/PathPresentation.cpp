#include "PathPresentation.hpp"

#include <cch/tui/TerminalImage.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {
namespace {

/// pi `utils/paths.ts:7` `UNICODE_SPACES`
/// `/[\u00A0\u2000-\u200A\u202F\u205F\u3000]/g`, as the UTF-8 byte sequences
/// those fifteen code points encode to. Every match becomes one ASCII space.
constexpr std::array<std::string_view, 15> kUnicodeSpaceBytes{
        "\xC2\xA0",    // U+00A0 no-break space
        "\xE2\x80\x80", // U+2000 en quad
        "\xE2\x80\x81", // U+2001 em quad
        "\xE2\x80\x82", // U+2002 en space
        "\xE2\x80\x83", // U+2003 em space
        "\xE2\x80\x84", // U+2004 three-per-em space
        "\xE2\x80\x85", // U+2005 four-per-em space
        "\xE2\x80\x86", // U+2006 six-per-em space
        "\xE2\x80\x87", // U+2007 figure space
        "\xE2\x80\x88", // U+2008 punctuation space
        "\xE2\x80\x89", // U+2009 thin space
        "\xE2\x80\x8A", // U+200A hair space
        "\xE2\x80\xAF", // U+202F narrow no-break space
        "\xE2\x81\x9F", // U+205F medium mathematical space
        "\xE3\x80\x80", // U+3000 ideographic space
};

/// pi `utils/paths.ts:77-79` `normalizePath`'s `normalizeUnicodeSpaces` step.
[[nodiscard]] std::string normalize_unicode_spaces(std::string_view path) {
    std::string normalized;
    normalized.reserve(path.size());
    std::size_t at = 0;
    while (at < path.size()) {
        const auto* replacement = std::ranges::find_if(kUnicodeSpaceBytes, [&path, at](std::string_view space) {
            return path.substr(at).starts_with(space);
        });
        if (replacement == kUnicodeSpaceBytes.end()) {
            normalized.push_back(path[at]);
            ++at;
            continue;
        }
        normalized.push_back(' ');
        at += replacement->size();
    }
    return normalized;
}

/// pi `utils/paths.ts:80-82` `normalizePath`'s `stripAtPrefix` step: one
/// leading `@`, the CLI @file marker.
[[nodiscard]] std::string strip_at_prefix(std::string path) {
    if (!path.starts_with('@')) return path;
    return path.substr(1);
}

/// pi `utils/paths.ts:87-93` `normalizePath`'s `expandTilde` step: a bare
/// `~` is the home directory and a `~/` prefix is the home directory joined
/// with the rest.
[[nodiscard]] std::string expand_home_marker(std::string path) {
    const auto home = home_directory();
    if (home.empty()) return path;
    if (path == "~") return home.string();
    if (path.starts_with("~/")) return (home / path.substr(2)).string();
    return path;
}

/// The lexical tail both pi resolvers end with, and the seam's only path
/// arithmetic: an absolute path is honored as is, a relative one resolves
/// against the tool execution's cwd, and `..` is normalized rather than
/// rejected. `harness::WorkspaceFileSystem` resolves the same way for
/// filesystem access; this is its presentation-side twin, reached from the
/// renderer's `cwd` instead of a workspace root.
[[nodiscard]] std::string resolve_against_cwd(std::string_view path, std::string_view cwd) {
    const std::filesystem::path candidate{path};
    if (candidate.is_absolute()) return candidate.lexically_normal().string();
    return (std::filesystem::path{cwd} / candidate).lexically_normal().string();
}

/// pi `utils/paths.ts:102-106` `resolvePath(path, cwd)` as pi's
/// `linkPath` calls it: `normalizePath` with **default** options, so the home
/// marker expands but the unicode spaces and the leading `@` do not. That is a
/// different function from `resolveToCwd` above, and the difference is pi's,
/// not an omission here.
[[nodiscard]] std::string resolve_path(std::string_view path, std::string_view cwd) {
    return resolve_against_cwd(expand_home_marker(std::string{path}), cwd);
}

/// Node `pathToFileURL` percent-encoding: the unreserved characters plus the
/// path-safe set stay literal and every other byte is percent-encoded as UTF-8.
/// This mirrors the encoding table of `cch::tui`'s private `path_to_file_url`
/// (`src/tui/TerminalImage.cpp`), which is not published interface; the two
/// must move together if either changes.
[[nodiscard]] std::string file_url_encode(std::string_view path) {
    std::string encoded;
    encoded.reserve(path.size());
    for (const auto byte : path) {
        const auto character = static_cast<unsigned char>(byte);
        const auto literal = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || byte == '-' || byte == '_' || byte == '.' ||
                             byte == '!' || byte == '*' || byte == '(' || byte == ')' || byte == '\'' || byte == ':' ||
                             byte == ';' || byte == '=' || byte == ',' || byte == '@' || byte == '&' || byte == '+' ||
                             byte == '$' || byte == '/';
        if (literal) {
            encoded.push_back(byte);
            continue;
        }
        encoded += std::format("%{:02X}", character);
    }
    return encoded;
}

} // namespace

std::filesystem::path home_directory() {
    const auto* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') return home;
    const auto* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && *profile != '\0') return profile;
    return {};
}

std::string resolve_to_cwd(std::string_view path, std::string_view cwd) {
    return resolve_against_cwd(expand_home_marker(strip_at_prefix(normalize_unicode_spaces(path))), cwd);
}

std::string shorten_path(std::string_view path) {
    const auto home = home_directory().string();
    if (home.empty() || path.size() < home.size()) return std::string{path};
    const std::string_view home_prefix{home};
    if (!path.starts_with(home_prefix)) return std::string{path};
    if (path == home_prefix) return "~";
    return "~" + std::string{path.substr(home_prefix.size())};
}

std::string render_tool_path(const LiveTheme& theme, std::string_view cwd, std::optional<std::string_view> raw_path) {
    if (!raw_path.has_value()) return theme.foreground(ThemeToken::Error, "[invalid arg]");
    const auto value = *raw_path;
    if (value.empty()) return theme.foreground(ThemeToken::ToolOutput, "...");
    const auto styled = theme.foreground(ThemeToken::Accent, shorten_path(value));
    if (!cch::tui::get_image_capabilities().hyperlinks) return styled;
    return cch::tui::hyperlink(styled, "file://" + file_url_encode(resolve_path(value, cwd)));
}

} // namespace cch::coding_agent::tui
