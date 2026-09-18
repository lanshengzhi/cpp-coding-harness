#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tests {

/// A background hook that wraps a padded line in one SGR pair, the way pi's
/// `applyBackgroundToLine` applies a background: `open` before the text and
/// `close` after it (SGR 49 by default).
[[nodiscard]] inline cch::tui::BackgroundHook background_hook(
        std::string_view open, std::string_view close = "\x1b[49m") {
    return [open = std::string(open), close = std::string(close)](
                   std::string text) { return std::format("{}{}{}", open, text, close); };
}

inline void check_background_cells(
        const cch::tui::VirtualTerminal& terminal, std::size_t rows, std::size_t columns, std::string_view color) {
    CHECK(terminal.cells().size() == rows);
    for (const auto& row : terminal.cells()) {
        CHECK(row.size() == columns);
        for (const auto& cell : row)
            CHECK(cell.style.bg_color == color);
    }
}

[[nodiscard]] inline std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                ++index;
            if (index < text.size()) ++index;
            continue;
        }
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == ']') {
            index += 2;
            while (index < text.size() && text[index] != '\a')
                ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] inline std::string rendered_screen(cch::tui::Component& component, std::size_t width = 80) {
    const auto rendered = component.render(width);
    REQUIRE(rendered);
    std::string text;
    for (const auto& line : rendered->lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

} // namespace cch::tests
