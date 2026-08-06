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

/// Shared width-module helpers (used by the public utility surface and by
/// `prepare_rendered_line`).
[[nodiscard]] inline util::Error invalid_terminal_text(std::string message, std::string detail = {}) {
    return util::make_error(util::ErrorCode::Validation, std::move(message), std::move(detail));
}

[[nodiscard]] inline std::string normalized_text(const std::vector<TerminalToken>& tokens) {
    std::string result;
    for (const auto& token : tokens) result += token.text;
    return result;
}

[[nodiscard]] inline util::Expected<std::size_t> token_width(const std::vector<TerminalToken>& tokens) {
    std::size_t width = 0;
    for (const auto& token : tokens) {
        if (token.kind == TerminalTokenKind::Newline) {
            return std::unexpected(invalid_terminal_text("Rendered terminal line contains a newline"));
        }
        width += token.width;
    }
    return width;
}

[[nodiscard]] std::pair<char32_t, std::size_t> decode_utf8(
    std::string_view text,
    std::size_t position);

[[nodiscard]] util::Expected<std::string> normalize_terminal_output(std::string_view text);

[[nodiscard]] util::Expected<std::string> prepare_rendered_line(
    std::string_view line,
    std::size_t width);

} // namespace cch::tui::detail
