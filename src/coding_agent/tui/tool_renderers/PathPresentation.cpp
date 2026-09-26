#include "PathPresentation.hpp"

#include <cch/tui/TerminalImage.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {
namespace {

/// pi `os.homedir()` as this repository reads it elsewhere
/// (`Footer.cpp`, `cch::tui`'s image-path shortening).
[[nodiscard]] std::string home_directory() {
    const auto* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') return home;
    const auto* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && *profile != '\0') return profile;
    return {};
}

/// pi `utils/paths.ts` `resolvePath(path, cwd)`: an absolute path is honored as
/// is, a relative one resolves against the tool execution's cwd. Lexical
/// normalization only, so the presentation seam performs no filesystem I/O.
[[nodiscard]] std::string resolve_path(std::string_view path, std::string_view cwd) {
    const std::filesystem::path candidate{path};
    if (candidate.is_absolute()) return candidate.lexically_normal().string();
    return (std::filesystem::path{cwd} / candidate).lexically_normal().string();
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

std::string shorten_path(std::string_view path) {
    const auto home = home_directory();
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
