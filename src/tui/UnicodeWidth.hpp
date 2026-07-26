#pragma once

#include <cch/util/Error.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {

/// Width of a single Unicode codepoint in terminal columns.
[[nodiscard]] int codepoint_width(char32_t cp);

/// Width of a single grapheme cluster in terminal columns.
/// Handles combining marks, emoji sequences, and regional indicators.
[[nodiscard]] int grapheme_width(std::string_view cluster);

/// Visible width of a full string in terminal columns.
/// Strips ANSI/OSC/APC escape sequences and expands tabs.
[[nodiscard]] int visible_width(std::string_view text);

/// Extract the first ANSI/OSC/APC escape sequence at position @p pos.
/// Returns {code, length} or nullopt if no escape is at @p pos.
struct AnsiCode {
    std::string code;
    std::size_t length;
};
[[nodiscard]] std::optional<AnsiCode> extract_ansi_code(std::string_view text, std::size_t pos);

/// Split a string into grapheme clusters (minimal terminal-visible units).
[[nodiscard]] std::vector<std::string> split_graphemes(std::string_view text);

/// Structure returned by ANSI code tracking across lines.
struct AnsiStyleState {
    bool bold{false};
    bool dim{false};
    bool italic{false};
    bool underline{false};
    bool blink{false};
    bool inverse{false};
    bool hidden{false};
    bool strikethrough{false};
    std::string fg_color; // SGR parameter string like "31" or "38;5;240"
    std::string bg_color; // SGR parameter string like "41" or "48;5;240"
    std::string hyperlink; // OSC 8 URL, empty if no hyperlink
    std::string hyperlink_params; // OSC 8 params

    [[nodiscard]] std::string get_active_codes() const;
    [[nodiscard]] bool has_active_codes() const;
    [[nodiscard]] std::string get_line_end_reset() const;
    void process_ansi(std::string_view code);
    void reset();
};

/// Word-wrap text to fit within @p width visible columns.
/// Preserves ANSI styling across line breaks.
[[nodiscard]] util::Expected<std::vector<std::string>> wrap_text(
    std::string_view text,
    int width);

/// Truncate text to fit within @p max_width visible columns.
/// Appends @p ellipsis when truncation occurs.
/// Optionally pads with spaces to exactly @p max_width.
[[nodiscard]] util::Expected<std::string> truncate_text(
    std::string_view text,
    int max_width,
    std::string_view ellipsis = "...",
    bool pad = false);

/// Decode one UTF-8 codepoint at position @p pos.
/// Returns (codepoint, bytes_consumed). On malformed input, returns (0xFFFD, 1).
[[nodiscard]] std::pair<char32_t, int> decode_utf8(std::string_view text, std::size_t pos);

/// Normalize text for terminal output:
/// - Expands tabs to 3 spaces outside ANSI sequences
/// - Replaces malformed UTF-8 with U+FFFD
/// - Rejects unsupported C0 control characters (except tab, LF)
[[nodiscard]] util::Expected<std::string> normalize_terminal_output(std::string_view text);

} // namespace cch::tui::detail
