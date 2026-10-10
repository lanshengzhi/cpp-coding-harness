#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::tui {

struct IndexedColor {
    std::uint8_t index{0};

    bool operator==(const IndexedColor&) const = default;
};

struct RgbColor {
    double red{0};
    double green{0};
    double blue{0};

    bool operator==(const RgbColor&) const = default;
};

using Color = std::variant<IndexedColor, RgbColor>;

enum class TerminalColorMode {
    Xterm256,
    TrueColor,
};

/// SGR attributes and optional terminal colors for one styled text span.
struct TextStyle {
    std::optional<Color> foreground;
    std::optional<Color> background;
    bool bold{false};
    bool dim{false};
    bool italic{false};
    bool underline{false};
    bool inverse{false};
    bool strikethrough{false};
};

/// Indexed palette channels match frozen pi's `colors.ts` BASIC_COLORS for indexes 0-15.
[[nodiscard]] support::Expected<IndexedColor> indexed_color(int index);

/// Construct an RGB color after checking each frozen pi 0..255 channel range.
[[nodiscard]] support::Expected<RgbColor> rgb_color(double red, double green, double blue);

/// Parse an indexed color or a #RGB/#RRGGBB hexadecimal string (pi `parseColor`).
[[nodiscard]] support::Expected<Color> parse_color(int index);
[[nodiscard]] support::Expected<Color> parse_color(double index);
[[nodiscard]] support::Expected<Color> parse_color(std::string_view value);

/// Convert a validated color to sRGB channels or canonical lowercase hex.
[[nodiscard]] support::Expected<RgbColor> color_to_rgb(const Color& color);
[[nodiscard]] support::Expected<std::string> color_to_hex(const Color& color);

/// Format a foreground/background SGR sequence for the terminal's color mode.
[[nodiscard]] support::Expected<std::string> foreground_ansi(const Color& color, TerminalColorMode mode);
[[nodiscard]] support::Expected<std::string> background_ansi(const Color& color, TerminalColorMode mode);

/// Apply colors and SGR attributes around text, retaining existing ANSI and OSC 8 bytes.
[[nodiscard]] support::Expected<std::string> style_text(
        std::string_view text, const TextStyle& style, TerminalColorMode mode);

/// Apply precomputed foreground/background SGR sequences. The colors in style are ignored.
[[nodiscard]] std::string style_text_with_ansi(std::string_view text,
        std::optional<std::string_view> foreground,
        std::optional<std::string_view> background,
        const TextStyle& style = {});

/// Visible terminal width of `text` in columns. Tabs count as 3 columns; ANSI,
/// OSC 8 hyperlinks, and combining marks contribute nothing; newlines contribute
/// nothing and do not reset the accumulator (frozen pi `visibleWidth` sums every
/// grapheme across logical lines).
[[nodiscard]] std::size_t visible_width(std::string_view text);

/// Word-wrap `text` to at most `width` visible columns per line, preserving
/// ANSI styling across physical line breaks (pi `wrapTextWithAnsi`).
[[nodiscard]] support::Expected<std::vector<std::string>> wrap_text(std::string_view text, std::size_t width);

/// Truncate `text` to at most `max_width` visible columns, appending
/// `ellipsis` when truncation occurs and optionally padding to exactly
/// `max_width` (pi `truncateToWidth`).
[[nodiscard]] support::Expected<std::string> truncate_text(
        std::string_view text, std::size_t max_width, std::string_view ellipsis = "...", bool pad = false);

/// Extract the text occupying visible columns `[start_col, start_col + length)`
/// of `line`, carrying ANSI styling into the result. With `strict`, a wide
/// grapheme extending past the range end is excluded (pi `sliceByColumn`).
[[nodiscard]] support::Expected<std::string> slice_by_column(
        std::string_view line, std::size_t start_col, std::size_t length, bool strict = false);

/// The OSC 8 hyperlink covering visible column `column` of `line`, or nothing
/// when no link is open there (pi `getOsc8LinkAtColumn`). A wide grapheme
/// covers every column it occupies, so a CJK character reports the same link at
/// each of its cells.
[[nodiscard]] std::optional<std::string> osc8_link_at_column(std::string_view line, std::size_t column);

/// Remove ANSI, OSC, and APC control sequences from `text`, preserving the
/// visible text (pi `stripTerminalSequences`).
[[nodiscard]] std::string strip_terminal_sequences(std::string_view text);

/// Text already present immediately before and after a paste insertion point.
/// Only the character nearest the insertion point is inspected, so callers may
/// pass any suffix of the preceding line and any prefix of the following one.
struct PasteBoundaries {
    std::string_view text_before{};
    std::string_view text_after{};
};

/// Passive options describing the insertion target of a paste (pi's
/// `Editor`/`Input` `handlePaste` differ exactly in `multiline`).
struct PasteNormalization {
    bool multiline{true};
    PasteBoundaries boundaries{};
};

/// Normalize pasted content before it is inserted, as frozen pi does:
///
/// - CR, CRLF and lone LF all collapse to one LF, kept in a multiline target
///   and dropped in a single-line one;
/// - TAB expands to four columns in both targets;
/// - a complete Kitty `CSI u` control reply inside the paste resolves to the
///   character it encodes (`ESC [ 106 ; 5 u` is Ctrl+J, a newline) instead of
///   leaking its printable parameter text;
/// - remaining C0, C1 and DEL control bytes are filtered out;
/// - a pasted file path that directly follows a word character (in the text
///   before it, or earlier in the same paste) gains one separating space, and
///   the same applies to the path's end against following text.
///
/// The result is ordinary UTF-8 text suitable for idiomatic C++ storage.
[[nodiscard]] std::string normalize_pasted_text(
        std::string_view text, PasteNormalization normalization = PasteNormalization{});

} // namespace cch::tui
