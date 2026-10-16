#include "EditorLayout.hpp"

#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Utils.hpp>
#include "tui/CompletionContext.hpp"
#include "tui/InteractionUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

constexpr std::size_t kDefaultPrimaryColumnWidth = 32;
constexpr std::size_t kPrimaryColumnGap = 2;
constexpr std::size_t kMinDescriptionWidth = 10;

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
    line += "\x1b[7m \x1b[27m";
}

[[nodiscard]] std::string single_line(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\r' || text[index] == '\n') {
            result.push_back(' ');
            do {
                ++index;
            } while (index < text.size() && (text[index] == '\r' || text[index] == '\n'));
            continue;
        }
        const auto [codepoint, length] = decode_utf8(text, index);
        (void)codepoint;
        result.append(text, index, length);
        index += length;
    }
    return std::string(detail::trim_completion_whitespace(result));
}

support::Expected<std::string> render_autocomplete_item(const AutocompleteItem& item,
        bool selected,
        std::size_t width,
        std::size_t primary_column_width,
        SelectListTheme& theme) {
    constexpr std::size_t prefix_width = 2;
    const std::string prefix = selected ? "→ " : "  ";
    const auto label = item.label.empty() ? std::string_view{item.value} : std::string_view{item.label};
    const auto description = single_line(item.description);
    std::string line;
    if (!description.empty() && width > 40) {
        const auto effective_primary = std::max<std::size_t>(
                1, std::min(primary_column_width, width > prefix_width + 4 ? width - prefix_width - 4 : 1));
        const auto primary_limit = std::max<std::size_t>(1, effective_primary - kPrimaryColumnGap);
        auto primary = truncate_text(label, primary_limit, "");
        if (!primary) return std::unexpected(primary.error());
        const auto primary_visible = visible_width(*primary);
        const auto spacing_size = std::max<std::size_t>(1, effective_primary - primary_visible);
        const auto description_start = prefix_width + primary_visible + spacing_size;
        const auto remaining = width > description_start + 2 ? width - description_start - 2 : 0;
        if (remaining > kMinDescriptionWidth) {
            auto truncated_description = truncate_text(description, remaining, "");
            if (!truncated_description) return std::unexpected(truncated_description.error());
            const auto spacing = std::string(spacing_size, ' ');
            if (selected) {
                line = prefix + *primary + spacing + *truncated_description;
            } else {
                auto styled_description = detail::apply_text_style(
                        theme.description, spacing + *truncated_description, "Editor autocomplete description");
                if (!styled_description) return std::unexpected(styled_description.error());
                line = prefix + *primary + *styled_description;
            }
        }
    }
    if (line.empty()) {
        const auto available = width > prefix_width + 2 ? width - prefix_width - 2 : 0;
        auto primary = truncate_text(label, available, "");
        if (!primary) return std::unexpected(primary.error());
        line = prefix + *primary;
        auto bounded = truncate_text(line, width, "");
        if (!bounded) return std::unexpected(bounded.error());
        line = std::move(*bounded);
    }
    if (selected) {
        auto styled =
                detail::apply_text_style(theme.selected_text, std::move(line), "Editor autocomplete selected item");
        if (!styled) return std::unexpected(styled.error());
        line = std::move(*styled);
    }
    return line;
}

support::ExpectedVoid append_autocomplete_lines(const EditorCompletionMenuPresentation& menu,
        std::size_t available_height,
        std::size_t content_width,
        std::size_t padding,
        std::size_t max_visible,
        SelectListTheme& theme,
        std::vector<std::string>& result) {
    if (!menu.open || menu.items.empty()) return {};
    const auto text_lines_count = result.size();
    const auto remainder_height = available_height > text_lines_count ? available_height - text_lines_count : 0;
    auto item_capacity = std::min(max_visible, remainder_height);
    if (item_capacity == 0) return {};
    if (menu.items.size() > item_capacity && item_capacity == remainder_height && remainder_height > 1) {
        --item_capacity;
    }
    const auto range = detail::centered_visible_range(menu.items.size(), menu.selected_index, item_capacity);
    const bool counter_owed = range.begin > 0 || range.end < menu.items.size();
    std::size_t primary_column_width = kDefaultPrimaryColumnWidth;
    if (menu.prefix.starts_with('/')) {
        std::size_t widest = 0;
        for (const auto& item : menu.items) {
            const auto label = item.label.empty() ? std::string_view{item.value} : std::string_view{item.label};
            widest = std::max(widest, visible_width(label) + kPrimaryColumnGap);
        }
        primary_column_width = std::clamp<std::size_t>(widest, 12, 32);
    }
    for (std::size_t index = range.begin; index < range.end; ++index) {
        auto rendered = render_autocomplete_item(
                menu.items[index], index == menu.selected_index, content_width, primary_column_width, theme);
        if (!rendered) return std::unexpected(rendered.error());
        std::string line(padding, ' ');
        line += std::move(*rendered);
        const auto line_width = visible_width(line);
        if (line_width < content_width + padding * 2) line.append(content_width + padding * 2 - line_width, ' ');
        result.push_back(std::move(line));
    }
    if (counter_owed && range.end - range.begin < remainder_height) {
        auto counter = truncate_text(std::format("  ({}/{})", menu.selected_index + 1, menu.items.size()),
                content_width > 2 ? content_width - 2 : 0,
                "");
        if (!counter) return std::unexpected(counter.error());
        auto styled =
                detail::apply_text_style(theme.scroll_info, std::move(*counter), "Editor autocomplete scroll info");
        if (!styled) return std::unexpected(styled.error());
        std::string line(padding, ' ');
        line += std::move(*styled);
        const auto line_width = visible_width(line);
        if (line_width < content_width + padding * 2) line.append(content_width + padding * 2 - line_width, ' ');
        result.push_back(std::move(line));
    }
    return {};
}

} // namespace

EditorContentWidth EditorLayout::calculate_content_width(
        std::size_t outer_width, std::size_t requested_padding) noexcept {
    if (outer_width == 0) return {};

    const auto max_padding = (outer_width - 1) / 2;
    const auto padding = std::min(requested_padding, max_padding);
    const auto content = std::max<std::size_t>(1, outer_width - padding * 2);
    const auto layout = std::max<std::size_t>(1, content - (padding == 0 ? 1 : 0));
    return {.padding = padding, .content = content, .layout = layout};
}

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
    struct IndexedGrapheme {
        std::string text;
        std::size_t segment;
    };

    std::vector<EditorVisualLine> result;
    for (std::size_t line_index = 0; line_index < document.size(); ++line_index) {
        const auto& line = document[line_index];
        if (line.empty()) {
            result.push_back({.logical_line = line_index, .start = 0, .end = 0, .text = {}});
            continue;
        }

        std::string line_text;
        std::vector<IndexedGrapheme> graphemes;
        for (std::size_t segment = 0; segment < line.size(); ++segment) {
            line_text += line[segment].text;
            for (auto& grapheme : split_graphemes(line[segment].text)) {
                graphemes.push_back({.text = std::move(grapheme), .segment = segment});
            }
        }

        auto wrapped = wrap_text(line_text, width);
        if (!wrapped) std::terminate();

        std::size_t source_index = 0;
        for (std::size_t wrapped_index = 0; wrapped_index < wrapped->size(); ++wrapped_index) {
            auto& wrapped_line = (*wrapped)[wrapped_index];
            if (wrapped_line.empty()) {
                const auto segment = source_index < graphemes.size() ? graphemes[source_index].segment : line.size();
                result.push_back({
                        .logical_line = line_index,
                        .start = segment,
                        .end = segment,
                        .text = {},
                });
                continue;
            }

            std::size_t start = line.size();
            std::size_t end = line.size();
            for (const auto& output_grapheme : split_graphemes(wrapped_line)) {
                while (source_index < graphemes.size() && graphemes[source_index].text != output_grapheme) {
                    ++source_index;
                }
                if (source_index == graphemes.size()) std::terminate();
                if (start == line.size()) start = graphemes[source_index].segment;
                end = graphemes[source_index].segment + 1;
                ++source_index;
            }
            if (wrapped_index + 1 == wrapped->size()) end = line.size();
            result.push_back({
                    .logical_line = line_index,
                    .start = start,
                    .end = end,
                    .text = std::move(wrapped_line),
            });
        }
    }
    return result;
}

std::optional<CursorPosition> EditorLayout::compute_cursor_position(const BufferDocument& document,
        std::span<const EditorVisualLine> visual,
        BufferCursor cursor,
        std::size_t rows_above_content,
        std::size_t scroll_offset,
        std::size_t visible_count,
        std::optional<std::size_t> cursor_line,
        std::size_t left_padding) {
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

    const std::size_t display_row = rows_above_content + visual_row - scroll_offset;

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
    return CursorPosition{.column = col + left_padding, .row = display_row};
}

support::Expected<EditorLayoutResult> EditorLayout::compute(EditorLayoutOptions options) {
    static const BufferDocument kEmptyDocument{BufferLine{}};
    const auto& doc = options.document ? *options.document : kEmptyDocument;

    const auto widths = calculate_content_width(options.width, options.padding_x);
    if (auto valid = validate_width(doc, widths.layout); !valid) {
        return std::unexpected(valid.error());
    }

    auto visual = construct_visual_lines(doc, widths.layout);

    std::size_t cursor_line = 0;
    for (std::size_t index = 0; index < visual.size(); ++index) {
        if (visual[index].logical_line == options.cursor.line && options.cursor.column >= visual[index].start &&
                options.cursor.column <= visual[index].end) {
            cursor_line = index;
            break;
        }
    }

    const std::size_t border_rows = (options.theme && options.theme->border) ? 2 : 0;
    const std::size_t top_border_rows = border_rows != 0 ? 1 : 0;
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
        if (options.top_border_override != nullptr) {
            auto overridden = (*options.top_border_override)(options.width, scroll_offset);
            if (!overridden) return std::unexpected(overridden.error());
            if (overridden->has_value()) {
                // The replacement arrives fully styled (pi CustomEditor
                // renderTopBorder composes from borderColor segments); no
                // further border styling applies.
                result.push_back(std::move(**overridden));
            } else {
                auto styled_border = apply_text_style(options.theme->border, std::move(top_border), "Editor border");
                if (!styled_border) return std::unexpected(styled_border.error());
                result.push_back(std::move(*styled_border));
            }
        } else {
            auto styled_border = apply_text_style(options.theme->border, std::move(top_border), "Editor border");
            if (!styled_border) return std::unexpected(styled_border.error());
            result.push_back(std::move(*styled_border));
        }
    }

    const auto end = std::min(visual.size(), scroll_offset + visible_count);
    const auto content_start_row = top_border_rows;
    const auto content_row_count = end > scroll_offset ? end - scroll_offset : 0;
    for (std::size_t index = scroll_offset; index < end; ++index) {
        auto content_line = visual[index].text;
        if (index == cursor_line) {
            insert_fake_cursor(visual[index], doc, options.cursor.column, content_line);
        }
        const auto content_line_width = visible_width(content_line);
        if (content_line_width < widths.content) content_line.append(widths.content - content_line_width, ' ');
        auto line = std::string(widths.padding, ' ') + content_line;
        const auto right_padding =
                widths.padding > 0 && content_line_width > widths.content ? widths.padding - 1 : widths.padding;
        line.append(right_padding, ' ');
        if (options.theme && options.theme->text) {
            auto styled = apply_text_style(options.theme->text, std::move(line), "Editor text");
            if (!styled) return std::unexpected(styled.error());
            result.push_back(std::move(*styled));
        } else {
            result.push_back(std::move(line));
        }
    }

    std::size_t fallback_content_rows = 0;
    if (result.size() == ((options.theme && options.theme->border) ? 1 : 0)) {
        fallback_content_rows = 1;
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

    const auto autocomplete_start_row = result.size();
    if (options.include_autocomplete && options.autocomplete_menu) {
        SelectListTheme default_select_list_theme;
        auto& select_list_theme = options.theme ? options.theme->select_list : default_select_list_theme;
        if (auto appended = append_autocomplete_lines(*options.autocomplete_menu,
                    options.available_height,
                    widths.content,
                    widths.padding,
                    options.autocomplete_max_visible,
                    select_list_theme,
                    result);
                !appended) {
            return std::unexpected(appended.error());
        }
    }

    std::optional<CursorPosition> cursor_position = compute_cursor_position(
            doc, visual, options.cursor, top_border_rows, scroll_offset, visible_count, cursor_line, widths.padding);

    return EditorLayoutResult{
            .lines = std::move(result),
            .padding_width = widths.padding,
            .content_width = widths.content,
            .layout_width = widths.layout,
            .scroll_offset = scroll_offset,
            .content_start_row = content_start_row,
            .content_row_count = content_row_count != 0 ? content_row_count : fallback_content_rows,
            .autocomplete_start_row = autocomplete_start_row,
            .autocomplete_row_count = result.size() - autocomplete_start_row,
            .cursor_position = cursor_position,
    };
}

} // namespace cch::tui::detail
