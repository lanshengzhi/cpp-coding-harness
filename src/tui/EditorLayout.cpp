#include "EditorLayout.hpp"

#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Utils.hpp>
#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

constexpr std::size_t kMaxAutocompleteRows = 5;

[[nodiscard]] std::string horizontal_rule(std::size_t width) {
    std::string rule;
    rule.reserve(width * 3);
    for (std::size_t index = 0; index < width; ++index)
        rule += "─";
    return rule;
}

[[nodiscard]] std::string scroll_border(std::string_view direction, std::size_t hidden_line_count, std::size_t width) {
    const auto indicator = std::format("─── {} {} more ", direction, hidden_line_count);
    const auto indicator_width = visible_width(indicator);
    if (indicator_width >= width) {
        const auto ellipsis = std::string{std::string_view{"..."}.substr(0, width)};
        const auto slice_width = width > visible_width(ellipsis) ? width - visible_width(ellipsis) : 0;
        auto sliced = slice_by_column(indicator, 0, slice_width, true);
        return (sliced ? *sliced : std::string{}) + ellipsis;
    }
    return indicator + horizontal_rule(width - indicator_width);
}

void insert_fake_cursor(const EditorVisualLine& visual_line,
        const BufferDocument& doc,
        std::size_t cursor_column,
        std::size_t width,
        std::string& line) {
    if (cursor_column < visual_line.end) {
        std::size_t byte_offset = 0;
        for (std::size_t index = visual_line.start; index < cursor_column; ++index) {
            byte_offset += doc[visual_line.logical_line][index].text.size();
        }
        const auto& segment_text = doc[visual_line.logical_line][cursor_column].text;
        const auto graphemes = split_graphemes(segment_text);
        const auto& at_cursor = graphemes.front();
        line.insert(byte_offset, "\x1b[7m");
        line.insert(byte_offset + 4 + at_cursor.size(), "\x1b[27m");
        return;
    }
    if (visible_width(line) < width) {
        line += "\x1b[7m \x1b[27m";
    }
}

support::ExpectedVoid append_autocomplete_lines(const EditorCompletionMenuPresentation& menu,
        std::size_t available_height,
        std::size_t width,
        std::vector<std::string>& result) {
    if (!menu.open || menu.items.empty()) return {};
    const auto text_lines_count = result.size();
    const auto remainder_height = available_height > text_lines_count ? available_height - text_lines_count : 0;
    const auto autocomplete_capacity = std::min(kMaxAutocompleteRows, remainder_height);
    if (autocomplete_capacity == 0) return {};
    const auto selected = menu.selected_index;
    const auto first_autocomplete = selected < autocomplete_capacity ? 0 : selected - autocomplete_capacity + 1;
    const auto autocomplete_count =
            std::min(autocomplete_capacity, menu.items.size() - std::min(first_autocomplete, menu.items.size()));
    for (std::size_t offset = 0; offset < autocomplete_count; ++offset) {
        const auto index = first_autocomplete + offset;
        std::string text = index == selected ? "> /" : "  /";
        text += menu.items[index].label;
        if (!menu.items[index].description.empty()) {
            text += " — " + menu.items[index].description;
        }
        TruncatedText item{std::move(text)};
        if (auto rendered = item.render(width); !rendered) {
            return std::unexpected(rendered.error());
        } else if (!rendered->lines.empty()) {
            result.push_back(std::move(rendered->lines.front()));
        }
    }
    return {};
}

} // namespace

support::ExpectedVoid EditorLayout::validate_width(const BufferDocument& document, std::size_t width) {
    if (width == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Editor requires a positive visible width"));
    }
    for (const auto& logical_line : document) {
        for (const auto& segment : logical_line) {
            for (const auto& grapheme : split_graphemes(segment.text)) {
                if (grapheme_width(grapheme) > width) {
                    return std::unexpected(support::make_error(support::ErrorCode::Validation,
                            "Editor grapheme is wider than the available visible width"));
                }
            }
        }
    }
    return {};
}

std::vector<EditorVisualLine> EditorLayout::construct_visual_lines(const BufferDocument& document, std::size_t width) {
    std::vector<EditorVisualLine> result;
    for (std::size_t line_index = 0; line_index < document.size(); ++line_index) {
        const auto& line = document[line_index];
        if (line.empty()) {
            result.push_back({.logical_line = line_index, .start = 0, .end = 0, .text = {}});
            continue;
        }
        std::size_t start = 0;
        std::size_t used = 0;
        std::string rendered;
        for (std::size_t index = 0; index < line.size(); ++index) {
            const auto segment_width = visible_width(line[index].text);
            if (used != 0 && used + segment_width > width) {
                result.push_back(
                        {.logical_line = line_index, .start = start, .end = index, .text = std::move(rendered)});
                start = index;
                used = 0;
                rendered.clear();
            }
            if (segment_width > width) {
                for (const auto& grapheme : split_graphemes(line[index].text)) {
                    if (used != 0 && used + grapheme_width(grapheme) > width) {
                        result.push_back({.logical_line = line_index,
                                .start = start,
                                .end = index,
                                .text = std::move(rendered)});
                        rendered.clear();
                        used = 0;
                    }
                    rendered += grapheme;
                    used += grapheme_width(grapheme);
                }
            } else {
                rendered += line[index].text;
                used += segment_width;
            }
        }
        result.push_back({.logical_line = line_index, .start = start, .end = line.size(), .text = std::move(rendered)});
    }
    return result;
}

std::optional<CursorPosition> EditorLayout::compute_cursor_position(const BufferDocument& document,
        std::span<const EditorVisualLine> visual,
        BufferCursor cursor,
        std::size_t border_rows,
        std::size_t scroll_offset,
        std::size_t visible_count,
        std::optional<std::size_t> cursor_line) {
    if (visual.empty()) return std::nullopt;

    std::size_t visual_row = 0;
    if (cursor_line && *cursor_line < visual.size() && visual[*cursor_line].logical_line == cursor.line &&
            cursor.column >= visual[*cursor_line].start && cursor.column <= visual[*cursor_line].end) {
        visual_row = *cursor_line;
    } else {
        bool found = false;
        for (std::size_t index = 0; index < visual.size(); ++index) {
            if (visual[index].logical_line == cursor.line && cursor.column >= visual[index].start &&
                    cursor.column <= visual[index].end) {
                visual_row = index;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }

    if (visual_row < scroll_offset || visual_row >= scroll_offset + visible_count) {
        return std::nullopt;
    }

    const std::size_t display_row = border_rows + visual_row - scroll_offset;

    const auto& vl = visual[visual_row];
    const std::size_t vl_text_width = visible_width(vl.text);
    const std::size_t cursor_in_line = cursor.column - vl.start;
    const std::size_t segs_in_line = vl.end - vl.start;
    std::size_t col = 0;
    if (segs_in_line > 0 && cursor_in_line <= segs_in_line) {
        const std::size_t seg_end = vl.start + cursor_in_line;
        if (vl.logical_line < document.size()) {
            const auto& line = document[vl.logical_line];
            for (std::size_t i = vl.start; i < seg_end && i < line.size(); ++i) {
                col += visible_width(line[i].text);
            }
        }
        col = std::min(col, vl_text_width);
    }
    return CursorPosition{.column = col, .row = display_row};
}

support::Expected<EditorLayoutResult> EditorLayout::compute(EditorLayoutOptions options) {
    static const BufferDocument kEmptyDocument{BufferLine{}};
    const auto& doc = options.document ? *options.document : kEmptyDocument;

    if (auto valid = validate_width(doc, options.width); !valid) {
        return std::unexpected(valid.error());
    }

    auto visual = construct_visual_lines(doc, options.width);

    std::size_t cursor_line = 0;
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == options.cursor.line && options.cursor.column >= visual[index].start &&
                options.cursor.column <= visual[index].end) {
            cursor_line = index;
            break;
        }
    }

    const std::size_t border_rows = (options.theme && options.theme->border) ? 2 : 0;
    const auto bordered = options.available_height > border_rows ? options.available_height - border_rows : 1;
    const auto visible_count = std::max<std::size_t>(1, std::min(options.max_visible_lines, bordered));

    std::size_t scroll_offset = options.scroll_offset;
    if (cursor_line < scroll_offset) scroll_offset = cursor_line;
    if (cursor_line >= scroll_offset + visible_count) {
        scroll_offset = cursor_line + 1 - visible_count;
    }

    std::vector<std::string> result;
    if (options.theme && options.theme->border) {
        auto top_border =
                scroll_offset > 0 ? scroll_border("↑", scroll_offset, options.width) : horizontal_rule(options.width);
        auto styled_border = apply_text_style(options.theme->border, std::move(top_border), "Editor border");
        if (!styled_border) return std::unexpected(styled_border.error());
        result.push_back(std::move(*styled_border));
    }

    const auto end = std::min(visual.size(), scroll_offset + visible_count);
    for (std::size_t index = scroll_offset; index < end; ++index) {
        auto line = visual[index].text;
        if (index == cursor_line) {
            insert_fake_cursor(visual[index], doc, options.cursor.column, options.width, line);
        }
        const auto line_width = visible_width(line);
        if (line_width < options.width) line.append(options.width - line_width, ' ');
        if (options.theme && options.theme->text) {
            auto styled = apply_text_style(options.theme->text, std::move(line), "Editor text");
            if (!styled) return std::unexpected(styled.error());
            result.push_back(std::move(*styled));
        } else {
            result.push_back(std::move(line));
        }
    }

    if (result.size() == ((options.theme && options.theme->border) ? 1 : 0)) {
        auto line = std::string(options.width, ' ');
        if (options.theme && options.theme->text) {
            auto styled = apply_text_style(options.theme->text, std::move(line), "Editor text");
            if (!styled) return std::unexpected(styled.error());
            result.push_back(std::move(*styled));
        } else {
            result.push_back(std::move(line));
        }
    }

    if (options.theme && options.theme->border) {
        const auto shown = scroll_offset + visible_count;
        const auto lines_below = visual.size() > shown ? visual.size() - shown : 0;
        auto bottom_border =
                lines_below > 0 ? scroll_border("↓", lines_below, options.width) : horizontal_rule(options.width);
        auto styled_border = apply_text_style(options.theme->border, std::move(bottom_border), "Editor border");
        if (!styled_border) return std::unexpected(styled_border.error());
        result.push_back(std::move(*styled_border));
    }

    if (options.include_autocomplete && options.autocomplete_menu) {
        if (auto appended = append_autocomplete_lines(
                    *options.autocomplete_menu, options.available_height, options.width, result);
                !appended) {
            return std::unexpected(appended.error());
        }
    }

    std::optional<CursorPosition> cursor_position = compute_cursor_position(
            doc, visual, options.cursor, border_rows, scroll_offset, visible_count, cursor_line);

    return EditorLayoutResult{
            .lines = std::move(result),
            .scroll_offset = scroll_offset,
            .cursor_position = cursor_position,
    };
}

} // namespace cch::tui::detail
