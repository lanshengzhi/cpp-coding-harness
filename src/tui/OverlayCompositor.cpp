#include "tui/OverlayCompositor.hpp"

#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tui::detail {
namespace {

/// The composed row under construction. `style` and `width` describe what
/// has been appended so far, so the row's trailing width bound and its
/// line-end reset are answered from the splice itself instead of by
/// re-parsing the string the splice just built.
struct ComposedRow {
    std::string text;
    AnsiStyleState style;
    std::size_t width{0};
    /// A rendered line that spans rows has no representation inside one
    /// physical row, so a splice that retains one is a malformed render.
    bool spans_rows{false};

    /// Append one control sequence carried by a token.
    void append_code(std::string_view code) {
        if (code.empty()) return;
        text += code;
        style.process_ansi(code);
    }

    /// Append a synthesized code run. `AnsiStyleState`'s getters return
    /// concatenated sequences while `process_ansi` consumes one sequence at
    /// a time, so the run is split the way a tokenization of the row would
    /// split it. The run is a well-formed sequence list by construction.
    void append_style_codes(std::string_view codes) {
        if (codes.empty()) return;
        text += codes;
        const auto tokens = tokenize_terminal_output(codes);
        if (!tokens) std::terminate();
        for (const auto& token : *tokens) {
            if (token.kind != TerminalTokenKind::Grapheme) style.process_ansi(token.text);
        }
    }

    void append_grapheme(std::string_view grapheme, std::size_t columns) {
        text += grapheme;
        width += columns;
    }

    void append_spaces(std::size_t count) {
        text.append(count, ' ');
        width += count;
    }
};

/// Append `source`'s first `limit` visible columns, padded out to `limit`:
/// the base row's contribution left of a spliced region. This is pi's
/// `extractSegments` "before" region (`compositeTuiLine`, tui.ts at the frozen
/// baseline) evaluated over the tokens the caller already holds, so the same
/// tokenization serves every piece of the splice.
void append_leading_columns(ComposedRow& row, const std::vector<TerminalToken>& source, std::size_t limit) {
    if (limit == 0) return;
    std::size_t source_width = 0;
    for (const auto& token : source)
        source_width += token.width;

    if (source_width <= limit) {
        // This region closes with the style in force over *this* source
        // alone, as `truncate_text` does — not with whatever the composed row
        // carried in from an earlier region, which `row.style` holds.
        AnsiStyleState style;
        for (const auto& token : source) {
            if (token.kind == TerminalTokenKind::Newline) row.spans_rows = true;
            if (token.kind == TerminalTokenKind::Grapheme) {
                row.append_grapheme(token.text, token.width);
                continue;
            }
            style.process_ansi(token.text);
            row.append_code(token.text);
        }
        row.append_style_codes(style.get_line_end_reset());
        row.append_spaces(limit - source_width);
        return;
    }

    std::size_t collected = 0;
    for (const auto& token : source) {
        if (token.kind != TerminalTokenKind::Grapheme) {
            if (token.kind == TerminalTokenKind::Newline) row.spans_rows = true;
            row.append_code(token.text);
            continue;
        }
        if (collected + token.width > limit) break;
        row.append_grapheme(token.text, token.width);
        collected += token.width;
    }
    // pi's `finalizeTruncatedResult` surrounds the ellipsis with a full
    // reset; the splice's ellipsis is empty, so one reset closes the region.
    row.append_style_codes(kSgrReset);
    row.append_spaces(limit - collected);
}

/// Append `source` from the visible column `cut` onward: the base row's
/// contribution right of a spliced region. Its own style tracker walks the
/// whole base row, so the codes reopened at `cut` are the ones in force
/// there rather than whatever the leading columns left behind.
void append_trailing_columns(ComposedRow& row, const std::vector<TerminalToken>& source, std::size_t cut) {
    AnsiStyleState style;
    std::size_t visible_column = 0;
    bool suffix_started = false;
    for (const auto& token : source) {
        if (token.kind != TerminalTokenKind::Grapheme) {
            if (token.kind == TerminalTokenKind::Newline) row.spans_rows = true;
            style.process_ansi(token.text);
            if (suffix_started) row.append_code(token.text);
            continue;
        }

        const auto next_column = visible_column + token.width;
        if (next_column <= cut) {
            visible_column = next_column;
            continue;
        }
        if (!suffix_started) {
            row.append_style_codes(style.get_active_codes());
            suffix_started = true;
        }
        if (visible_column < cut)
            row.append_spaces(next_column - cut);
        else
            row.append_grapheme(token.text, token.width);
        visible_column = next_column;
    }
    if (suffix_started) row.append_style_codes(style.get_line_end_reset());
}

/// Splice `replacement` over the base row's columns `[column, column +
/// columns)`, leaving a row exactly `total_width` columns wide.
///
/// The base row and the replacement are each tokenized once and walked once;
/// the three regions are emitted straight into the result. Re-deriving the
/// splice from the three finished strings instead would re-parse the base row
/// twice more and the assembled row once more per spliced row.
[[nodiscard]] support::Expected<std::string> replace_line_region(
    std::string_view line,
    std::string_view replacement,
    std::size_t column,
    std::size_t columns,
    std::size_t total_width) {
    auto line_tokens = tokenize_terminal_output(line);
    if (!line_tokens) return std::unexpected(line_tokens.error());
    auto replacement_tokens = tokenize_terminal_output(replacement);
    if (!replacement_tokens) return std::unexpected(replacement_tokens.error());

    ComposedRow row;
    append_leading_columns(row, *line_tokens, column);
    append_leading_columns(row, *replacement_tokens, columns);
    append_trailing_columns(row, *line_tokens, column + columns);
    if (row.spans_rows) {
        return std::unexpected(invalid_terminal_text("Rendered terminal line contains a newline"));
    }
    if (total_width == 0) return std::string{};
    if (row.width > total_width) {
        // Unreachable while a spliced region fits the viewport it is laid out
        // in; the bound keeps an over-wide base row from extending the row.
        return truncate_text(std::move(row.text), total_width, "", true);
    }
    row.append_style_codes(row.style.get_line_end_reset());
    row.append_spaces(total_width - row.width);
    return std::move(row.text);
}

} // namespace

support::Expected<std::reference_wrapper<Overlay>> OverlayCompositor::add_overlay(std::unique_ptr<Overlay> overlay,
        Component* pre_focus,
        std::size_t focus_order,
        std::shared_ptr<OverlayHandleState> handle_state) {
    if (!overlay) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI cannot attach a null Overlay"));
    }
    auto& ref = *overlay;
    const auto order = focus_order == 0 ? ++focus_order_counter_ : focus_order;
    focus_order_counter_ = std::max(focus_order_counter_, order);
    if (handle_state == nullptr) handle_state = std::make_shared<OverlayHandleState>();
    overlays_.push_back(OverlayEntry{
            .overlay = std::move(overlay),
            .pre_focus = pre_focus,
            .focus_order = order,
            .hidden = false,
            .bounds = std::nullopt,
            .handle_state = std::move(handle_state),
    });
    return ref;
}

support::ExpectedVoid OverlayCompositor::remove(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!owns(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }
    const auto before = overlays_.size();
    std::erase_if(overlays_, [overlay](const auto& entry) { return entry.overlay.get() == overlay; });
    if (overlays_.size() == before) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay not found in TUI"));
    }
    std::erase_if(focus_history_, [overlay](const auto& entry) { return entry.overlay == overlay; });
    return {};
}

bool OverlayCompositor::owns(const Overlay* overlay) const { return find_entry(overlay) != nullptr; }

bool OverlayCompositor::is_hidden(const Overlay* overlay) const {
    const auto* entry = find_entry(overlay);
    return entry == nullptr || entry->hidden;
}

bool OverlayCompositor::set_hidden(Overlay* overlay, bool hidden) {
    auto* entry = find_entry(overlay);
    if (entry == nullptr) return false;
    entry->hidden = hidden;
    entry->handle_state->hidden = hidden;
    if (hidden) entry->bounds.reset();
    return true;
}

bool OverlayCompositor::visible(const Overlay* overlay, TerminalDimensions viewport) const {
    const auto* entry = find_entry(overlay);
    return entry != nullptr && !entry->hidden && entry->overlay->visible_at(viewport);
}

Component* OverlayCompositor::pre_focus(const Overlay* overlay) const {
    const auto* entry = find_entry(overlay);
    return entry == nullptr ? nullptr : entry->pre_focus;
}

std::optional<OverlayBounds> OverlayCompositor::bounds(const Overlay* overlay) const {
    const auto* entry = find_entry(overlay);
    return entry == nullptr ? std::nullopt : entry->bounds;
}

void OverlayCompositor::set_focus_order(Overlay* overlay, std::size_t order) {
    focus_order_counter_ = std::max(focus_order_counter_, order);
    if (auto* entry = find_entry(overlay)) entry->focus_order = order;
}

void OverlayCompositor::retarget_pre_focus(Overlay* removed, Component* replacement) {
    for (auto& entry : overlays_) {
        if (entry.overlay.get() != removed && entry.pre_focus == removed) entry.pre_focus = replacement;
    }
}

OverlayCompositor::OverlayEntry* OverlayCompositor::find_entry(const Overlay* overlay) {
    const auto found = std::find_if(overlays_.begin(), overlays_.end(), [overlay](const auto& entry) {
        return entry.overlay.get() == overlay;
    });
    return found == overlays_.end() ? nullptr : &*found;
}

const OverlayCompositor::OverlayEntry* OverlayCompositor::find_entry(const Overlay* overlay) const {
    const auto found = std::find_if(overlays_.begin(), overlays_.end(), [overlay](const auto& entry) {
        return entry.overlay.get() == overlay;
    });
    return found == overlays_.end() ? nullptr : &*found;
}

void OverlayCompositor::invalidate_all() {
    for (const auto& entry : overlays_)
        entry.overlay->invalidate();
}

RenderResult OverlayCompositor::materialize_images(
    RenderResult output,
    const TerminalCapabilities& capabilities,
    std::size_t width,
    std::size_t available_rows) {
    auto candidates = std::move(output.images);
    output.images.clear();
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.region.row < right.region.row;
    });

    std::ptrdiff_t row_adjustment = 0;
    for (auto& image : candidates) {
        const auto adjusted_row = static_cast<std::ptrdiff_t>(image.region.row) + row_adjustment;
        if (adjusted_row < 0) continue;
        const auto target_row = static_cast<std::size_t>(adjusted_row);
        if (target_row >= output.lines.size()) continue;
        const auto source_rows = image.rows_reserved_in_lines
                                         ? std::min(image.region.rows, output.lines.size() - target_row)
                                         : std::size_t{1};
        const auto use_fallback = [&] {
            output.lines[target_row] = image.fallback_text;
            if (source_rows > 1) {
                output.lines.erase(output.lines.begin() + static_cast<std::ptrdiff_t>(target_row + 1),
                        output.lines.begin() + static_cast<std::ptrdiff_t>(target_row + source_rows));
                row_adjustment -= static_cast<std::ptrdiff_t>(source_rows - 1);
            }
        };
        if (!capabilities.cell_pixels || capabilities.cell_pixels->width == 0 ||
                capabilities.cell_pixels->height == 0 ||
                !protocol_supports_mime(capabilities.inline_images, image.mime_type) || image.pixel_width == 0 ||
                image.pixel_height == 0 || image.region.column >= width) {
            use_fallback();
            continue;
        }

        const auto available_width = width - image.region.column;
        const auto default_width = std::min<std::size_t>(width > 2 ? width - 2 : 1, 60);
        const auto max_width = std::max<std::size_t>(
            1,
            std::min(available_width, image.max_width.value_or(default_width)));
        const auto cells = *capabilities.cell_pixels;
        const auto default_height = std::max<std::size_t>(
            1,
            (max_width * cells.width + cells.height - 1) / cells.height);
        const auto max_height = std::max<std::size_t>(1, image.max_height.value_or(default_height));
        const auto size = calculate_image_cell_size({.width = image.pixel_width, .height = image.pixel_height},
                max_width,
                max_height,
                cells,
                capabilities.inline_images == InlineImageProtocol::Kitty);
        const auto columns = size.columns;
        const auto rows = size.rows;
        if (target_row >= available_rows || rows > available_rows - target_row) {
            use_fallback();
            continue;
        }

        if (rows < source_rows) {
            output.lines.erase(output.lines.begin() + static_cast<std::ptrdiff_t>(target_row + rows),
                    output.lines.begin() + static_cast<std::ptrdiff_t>(target_row + source_rows));
            row_adjustment -= static_cast<std::ptrdiff_t>(source_rows - rows);
        } else if (rows > source_rows) {
            output.lines.insert(output.lines.begin() + static_cast<std::ptrdiff_t>(target_row + source_rows),
                    rows - source_rows,
                    std::string{});
            row_adjustment += static_cast<std::ptrdiff_t>(rows - source_rows);
        }
        output.lines[target_row] = std::string(image.region.column + columns, ' ');
        image.region.columns = columns;
        image.region.rows = rows;
        image.region.row = target_row;
        output.images.push_back(std::move(image));
    }
    return output;
}

support::ExpectedVoid OverlayCompositor::composite(
        TerminalDimensions dimensions, const TerminalCapabilities& capabilities, RenderResult& output) {
    struct RenderedOverlay {
        RenderResult content;
        OverlayLayout layout;
    };

    output.cursor.reset();
    if (overlays_.empty()) return {};
    for (auto& entry : overlays_)
        entry.bounds.reset();

    std::vector<RenderedOverlay> rendered_overlays;
    std::size_t min_lines_needed = output.lines.size();
    for (const auto* entry : sorted_visible(dimensions)) {
        auto* overlay = entry->overlay.get();
        const auto initial_layout = overlay->resolve_layout(dimensions, 0);
        auto rendered = overlay->render(initial_layout.width);
        if (!rendered) return std::unexpected(rendered.error());
        auto materialized = materialize_images(std::move(*rendered),
                capabilities,
                initial_layout.width,
                initial_layout.max_height.value_or(dimensions.rows));
        const auto layout = overlay->resolve_layout(dimensions, materialized.lines.size());
        if (layout.max_height && materialized.lines.size() > *layout.max_height) {
            materialized.lines.resize(*layout.max_height);
            std::erase_if(materialized.images,
                    [&](const auto& image) { return image.region.row >= materialized.lines.size(); });
            if (materialized.cursor && materialized.cursor->row >= materialized.lines.size())
                materialized.cursor.reset();
        }
        if (layout.row <= std::numeric_limits<std::size_t>::max() - materialized.lines.size())
            min_lines_needed = std::max(min_lines_needed, layout.row + materialized.lines.size());
        entry->bounds = OverlayBounds{
                .row = layout.row,
                .col = layout.col,
                .width = layout.width,
                .height = materialized.lines.size(),
        };
        if (materialized.lines.empty()) continue;
        rendered_overlays.push_back({.content = std::move(materialized), .layout = layout});
    }

    const auto working_height = std::max({output.lines.size(), dimensions.rows, min_lines_needed});
    output.lines.resize(working_height);
    const auto viewport_start = working_height > dimensions.rows ? working_height - dimensions.rows : 0;

    for (auto& rendered : rendered_overlays) {
        auto& materialized = rendered.content;
        const auto& layout = rendered.layout;
        const auto final_col = layout.col;
        const auto final_row = layout.row;
        if (final_col >= dimensions.columns || final_row >= dimensions.rows) continue;

        const auto overlay_columns = dimensions.columns - final_col;
        const auto overlay_rows = dimensions.rows - final_row;
        std::erase_if(materialized.images, [&](const auto& image) {
            const auto fits_columns = image.region.column < overlay_columns &&
                image.region.columns <= overlay_columns - image.region.column;
            const auto fits_rows =
                    image.region.row < overlay_rows && image.region.rows <= overlay_rows - image.region.row;
            if (fits_columns && fits_rows) return false;
            if (image.region.row < materialized.lines.size()) {
                materialized.lines[image.region.row] = image.fallback_text;
            }
            return true;
        });

        const auto overlay_width = std::min(layout.width, overlay_columns);
        for (std::size_t row = 0; row < materialized.lines.size(); ++row) {
            if (final_row + row >= dimensions.rows || overlay_width == 0) break;
            const auto target_row = viewport_start + final_row + row;
            auto composited = replace_line_region(
                    output.lines[target_row], materialized.lines[row], final_col, overlay_width, dimensions.columns);
            if (!composited) return std::unexpected(composited.error());
            output.lines[target_row] = std::move(*composited);

            const CellRegion written{
                    .column = final_col,
                    .row = target_row,
                    .columns = overlay_width,
                    .rows = 1,
            };
            std::erase_if(
                    output.images, [&](const auto& image) { return cell_regions_intersect(image.region, written); });
        }

        if (materialized.cursor && materialized.cursor->row < materialized.lines.size()) {
            output.cursor = CursorPosition{
                    .column = final_col + materialized.cursor->column,
                    .row = viewport_start + final_row + materialized.cursor->row,
            };
        }

        for (auto& image : materialized.images) {
            image.region.column += final_col;
            image.region.row += viewport_start + final_row;
            const auto fits_columns = image.region.column < dimensions.columns &&
                                      image.region.columns <= dimensions.columns - image.region.column;
            const auto image_viewport_row = image.region.row - viewport_start;
            const auto fits_rows =
                    image_viewport_row < dimensions.rows && image.region.rows <= dimensions.rows - image_viewport_row;
            if (fits_columns && fits_rows) {
                output.images.push_back(std::move(image));
            } else if (image.region.row >= viewport_start && image.region.row < viewport_start + dimensions.rows) {
                const auto fallback_row = image.region.row;
                auto fallback_line = replace_line_region(
                        output.lines[fallback_row], image.fallback_text, final_col, overlay_width, dimensions.columns);
                if (!fallback_line) return std::unexpected(fallback_line.error());
                output.lines[fallback_row] = std::move(*fallback_line);
            }
        }
    }

    return {};
}

Overlay* OverlayCompositor::topmost_focusable(TerminalDimensions viewport) const {
    auto visible = sorted_visible(viewport);
    for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
        auto* overlay = (*it)->overlay.get();
        auto* focusable = dynamic_cast<Focusable*>(overlay);
        if (focusable != nullptr && !overlay->options().non_capturing) return overlay;
    }
    return nullptr;
}

bool OverlayCompositor::focus_target_available(Component* component, TerminalDimensions viewport) const {
    const auto* entry = find_entry(dynamic_cast<Overlay*>(component));
    return entry != nullptr && !entry->hidden && entry->overlay->visible_at(viewport) &&
           !entry->overlay->options().non_capturing;
}

void OverlayCompositor::remember_focus(Overlay* overlay, Component* previous) {
    const auto found = std::find_if(focus_history_.begin(), focus_history_.end(), [overlay](const auto& entry) {
        return entry.overlay == overlay;
    });
    if (found == focus_history_.end())
        focus_history_.push_back({.overlay = overlay, .previous = previous});
    else
        found->previous = previous;
}

Component* OverlayCompositor::return_focus(const Overlay* overlay) const {
    const auto found = std::find_if(focus_history_.begin(), focus_history_.end(), [overlay](const auto& entry) {
        return entry.overlay == overlay;
    });
    return found == focus_history_.end() ? nullptr : found->previous;
}

void OverlayCompositor::forget_focus(Overlay* overlay, Component* replacement) {
    retarget_pre_focus(overlay, replacement);
    for (auto& entry : focus_history_) {
        if (entry.previous == overlay) entry.previous = replacement;
    }
    std::erase_if(focus_history_, [overlay](const auto& entry) { return entry.overlay == overlay; });
}

void OverlayCompositor::clear_focus_history() { focus_history_.clear(); }

void OverlayCompositor::forget_component(Component* component) {
    for (auto& entry : overlays_) {
        if (entry.pre_focus == component) entry.pre_focus = nullptr;
    }
    for (auto& entry : focus_history_) {
        if (entry.previous == component) entry.previous = nullptr;
    }
}

InputAdmissionOutcome OverlayCompositor::dispatch_input(
        const InputEventVariant& event, TerminalDimensions viewport) const {
    auto visible = sorted_visible(viewport);
    for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
        auto* overlay = (*it)->overlay.get();
        if (overlay->options().non_capturing) continue;
        if (auto* handler = dynamic_cast<InputHandler*>(overlay);
                handler != nullptr && handler->handle_input(event) == InputAdmissionOutcome::Consumed)
            return InputAdmissionOutcome::Consumed;
    }
    return InputAdmissionOutcome::Unhandled;
}

bool OverlayCompositor::has_visible_overlays(TerminalDimensions viewport) const {
    return !sorted_visible(viewport).empty();
}

std::vector<const OverlayCompositor::OverlayEntry*> OverlayCompositor::sorted_visible(
        TerminalDimensions viewport) const {
    std::vector<const OverlayEntry*> visible;
    for (const auto& entry : overlays_) {
        if (!entry.hidden && entry.overlay->visible_at(viewport)) visible.push_back(&entry);
    }
    std::sort(visible.begin(), visible.end(), [](const OverlayEntry* left, const OverlayEntry* right) {
        if (left->overlay->options().z_index != right->overlay->options().z_index)
            return left->overlay->options().z_index < right->overlay->options().z_index;
        return left->focus_order < right->focus_order;
    });
    return visible;
}

} // namespace cch::tui::detail
