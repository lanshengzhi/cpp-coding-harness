#pragma once

#include <cch/tui/Editor.hpp>
#include <cch/tui/Style.hpp>
#include "tui/EditorCompletionSession.hpp"
#include "tui/TextBuffer.hpp"

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace cch::tui::detail {

struct EditorVisualLine {
    std::size_t logical_line{0};
    std::size_t start{0};
    std::size_t end{0};
    std::string text{};

    bool operator==(const EditorVisualLine&) const = default;
};

struct EditorLayoutOptions {
    const BufferDocument* document{nullptr};
    BufferCursor cursor{};
    std::size_t width{0};
    std::size_t max_visible_lines{5};
    std::size_t available_height{5};
    std::size_t scroll_offset{0};
    EditorTheme* theme{nullptr};
    const EditorCompletionMenuPresentation* autocomplete_menu{nullptr};
    bool include_autocomplete{true};
};

struct EditorLayoutResult {
    std::vector<std::string> lines{};
    std::size_t scroll_offset{0};
};

/// Private presentation calculator for Editor rendering. Owns visual line
/// construction, width validation, scroll and viewport calculation, borders,
/// padding, fake-cursor rendering, and the resulting render frame.
class EditorLayout final {
public:
    EditorLayout() = delete;

    [[nodiscard]] static support::ExpectedVoid validate_width(const BufferDocument& document, std::size_t width);

    [[nodiscard]] static std::vector<EditorVisualLine> construct_visual_lines(
            const BufferDocument& document, std::size_t width);

    [[nodiscard]] static support::Expected<EditorLayoutResult> compute(EditorLayoutOptions options);
};

} // namespace cch::tui::detail
