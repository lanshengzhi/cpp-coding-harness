#pragma once

#include <cch/tui/Editor.hpp>
#include <cch/tui/Style.hpp>
#include <cch/tui/Terminal.hpp>
#include "tui/EditorCompletionSession.hpp"
#include "tui/TextBuffer.hpp"

#include <cch/support/Error.hpp>

#include <cstddef>
#include <optional>
#include <span>
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
    std::size_t padding_x{0};
    std::size_t max_visible_lines{5};
    std::size_t available_height{5};
    std::size_t scroll_offset{0};
    EditorTheme* theme{nullptr};
    const EditorCompletionMenuPresentation* autocomplete_menu{nullptr};
    bool include_autocomplete{true};
    /// App-layer top border replacement (pi `CustomEditor.renderTopBorder`):
    /// borrowed from the owning Editor's options; must outlive the compute
    /// call. nullopt results keep the default border line.
    EditorTopBorderSink* top_border_override{nullptr};
};

struct EditorContentWidth {
    std::size_t padding{0};
    std::size_t content{0};
    std::size_t layout{0};
};

struct EditorLayoutResult {
    std::vector<std::string> lines{};
    std::size_t padding_width{0};
    std::size_t content_width{0};
    std::size_t layout_width{0};
    std::size_t scroll_offset{0};
    std::optional<CursorPosition> cursor_position{std::nullopt};
};

/// Private presentation calculator for Editor rendering. Owns visual line
/// construction, width validation, scroll and viewport calculation, borders,
/// padding, fake-cursor rendering, and the resulting render frame.
class EditorLayout final {
public:
    EditorLayout() = delete;

    [[nodiscard]] static EditorContentWidth calculate_content_width(
            std::size_t outer_width, std::size_t requested_padding) noexcept;

    [[nodiscard]] static support::ExpectedVoid validate_width(const BufferDocument& document, std::size_t width);

    [[nodiscard]] static std::vector<EditorVisualLine> construct_visual_lines(
            const BufferDocument& document, std::size_t width);

    [[nodiscard]] static std::optional<CursorPosition> compute_cursor_position(const BufferDocument& document,
            std::span<const EditorVisualLine> visual,
            BufferCursor cursor,
            std::size_t border_rows,
            std::size_t scroll_offset,
            std::size_t visible_count,
            std::optional<std::size_t> cursor_line = std::nullopt,
            std::size_t left_padding = 0);

    [[nodiscard]] static support::Expected<EditorLayoutResult> compute(EditorLayoutOptions options);
};

} // namespace cch::tui::detail
