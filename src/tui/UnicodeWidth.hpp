#pragma once

#include <cch/util/Error.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {

[[nodiscard]] std::size_t codepoint_width(char32_t codepoint);
[[nodiscard]] std::size_t grapheme_width(std::string_view cluster);
[[nodiscard]] std::size_t visible_width(std::string_view text);

struct AnsiCode {
    std::string code;
    std::size_t length{0};
};

[[nodiscard]] std::optional<AnsiCode> extract_ansi_code(std::string_view text, std::size_t position);
[[nodiscard]] std::vector<std::string> split_graphemes(std::string_view text);

struct AnsiStyleState {
    bool bold{false};
    bool dim{false};
    bool italic{false};
    bool underline{false};
    bool blink{false};
    bool inverse{false};
    bool hidden{false};
    bool strikethrough{false};
    std::string fg_color;
    std::string bg_color;
    std::string hyperlink;
    std::string hyperlink_params;

    [[nodiscard]] std::string get_active_codes() const;
    [[nodiscard]] bool has_active_codes() const;
    [[nodiscard]] bool has_sgr_codes() const;
    [[nodiscard]] std::string get_line_end_reset() const;
    void process_ansi(std::string_view code);
    void reset();
};

enum class TerminalTokenKind {
    Grapheme,
    Sgr,
    Hyperlink,
    Newline,
};

struct TerminalToken {
    TerminalTokenKind kind{TerminalTokenKind::Grapheme};
    std::string text;
    std::size_t width{0};
};

[[nodiscard]] util::Expected<std::vector<TerminalToken>> tokenize_terminal_output(std::string_view text);

[[nodiscard]] util::Expected<std::vector<std::string>> wrap_text(
    std::string_view text,
    std::size_t width);

[[nodiscard]] util::Expected<std::string> truncate_text(
    std::string_view text,
    std::size_t max_width,
    std::string_view ellipsis = "...",
    bool pad = false);

[[nodiscard]] std::pair<char32_t, std::size_t> decode_utf8(
    std::string_view text,
    std::size_t position);

[[nodiscard]] util::Expected<std::string> normalize_terminal_output(std::string_view text);

[[nodiscard]] util::Expected<std::string> prepare_rendered_line(
    std::string_view line,
    std::size_t width);

} // namespace cch::tui::detail
