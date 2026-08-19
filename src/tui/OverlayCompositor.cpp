#include "tui/OverlayCompositor.hpp"

#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tui::detail {
namespace {

[[nodiscard]] support::Expected<std::string> line_suffix_from_column(
    std::string_view line,
    std::size_t column) {
    auto tokens = tokenize_terminal_output(line);
    if (!tokens) return std::unexpected(tokens.error());

    AnsiStyleState style;
    std::string suffix;
    std::size_t visible_column = 0;
    bool suffix_started = false;
    for (const auto& token : *tokens) {
        if (token.kind != TerminalTokenKind::Grapheme) {
            style.process_ansi(token.text);
            if (suffix_started) suffix += token.text;
            continue;
        }

        const auto next_column = visible_column + token.width;
        if (next_column <= column) {
            visible_column = next_column;
            continue;
        }
        if (!suffix_started) {
            suffix += style.get_active_codes();
            suffix_started = true;
        }
        if (visible_column < column) suffix.append(next_column - column, ' ');
        else suffix += token.text;
        visible_column = next_column;
    }
    if (suffix_started) suffix += style.get_line_end_reset();
    return suffix;
}

[[nodiscard]] support::Expected<std::string> replace_line_region(
    std::string_view line,
    std::string_view replacement,
    std::size_t column,
    std::size_t columns,
    std::size_t total_width) {
    auto prefix = truncate_text(line, column, "", true);
    if (!prefix) return std::unexpected(prefix.error());
    auto bounded_replacement = truncate_text(replacement, columns, "", true);
    if (!bounded_replacement) return std::unexpected(bounded_replacement.error());
    auto suffix = line_suffix_from_column(line, column + columns);
    if (!suffix) return std::unexpected(suffix.error());
    return truncate_text(
        *prefix + *bounded_replacement + *suffix,
        total_width,
        "",
        true);
}

} // namespace

support::Expected<std::reference_wrapper<Overlay>> OverlayCompositor::add_overlay(
    std::unique_ptr<Overlay> overlay) {
    if (!overlay) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI cannot attach a null Overlay"));
    }
    auto& ref = *overlay;
    overlays_.push_back(std::move(overlay));
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
    std::erase_if(overlays_, [overlay](const auto& ptr) { return ptr.get() == overlay; });
    if (overlays_.size() == before) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay not found in TUI"));
    }
    return {};
}

bool OverlayCompositor::owns(const Overlay* overlay) const {
    for (const auto& owned : overlays_) {
        if (owned.get() == overlay) return true;
    }
    return false;
}

void OverlayCompositor::invalidate_all() {
    for (const auto& overlay : overlays_) {
        overlay->invalidate();
    }
}

RenderResult OverlayCompositor::materialize_images(
    RenderResult output,
    const TerminalCapabilities& capabilities,
    std::size_t width,
    std::size_t available_rows) {
    if (!capabilities.cell_pixels || capabilities.cell_pixels->width == 0 ||
        capabilities.cell_pixels->height == 0) {
        output.images.clear();
        return output;
    }

    auto candidates = std::move(output.images);
    output.images.clear();
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.region.row < right.region.row;
    });

    std::size_t added_rows = 0;
    for (auto& image : candidates) {
        if (!protocol_supports_mime(capabilities.inline_images, image.mime_type) ||
            image.pixel_width == 0 || image.pixel_height == 0 ||
            image.region.column >= width || image.region.row >= output.lines.size()) {
            continue;
        }

        const auto available_width = width - image.region.column;
        const auto default_width = std::min<std::size_t>(width > 2 ? width - 2 : 1, 60);
        const auto max_width = std::max<std::size_t>(
            1,
            std::min(available_width, image.max_width.value_or(default_width)));
        const auto& cells = *capabilities.cell_pixels;
        const auto default_height = std::max<std::size_t>(
            1,
            (max_width * cells.width + cells.height - 1) / cells.height);
        const auto max_height = std::max<std::size_t>(1, image.max_height.value_or(default_height));
        const auto width_scale =
            static_cast<long double>(max_width * cells.width) / image.pixel_width;
        const auto height_scale =
            static_cast<long double>(max_height * cells.height) / image.pixel_height;
        const auto scale = std::min(width_scale, height_scale);
        const auto columns = std::max<std::size_t>(
            1,
            std::min(max_width, static_cast<std::size_t>(std::ceil(
                static_cast<long double>(image.pixel_width) * scale / cells.width))));
        const auto rows = std::max<std::size_t>(
            1,
            std::min(max_height, static_cast<std::size_t>(std::ceil(
                static_cast<long double>(image.pixel_height) * scale / cells.height))));
        const auto target_row = image.region.row + added_rows;
        if (target_row >= available_rows || rows > available_rows - target_row) continue;

        output.lines[target_row] = std::string(image.region.column + columns, ' ');
        if (rows > 1) {
            output.lines.insert(
                output.lines.begin() + static_cast<std::ptrdiff_t>(target_row + 1),
                rows - 1,
                std::string{});
            added_rows += rows - 1;
        }
        image.region.columns = columns;
        image.region.rows = rows;
        image.region.row = target_row;
        output.images.push_back(std::move(image));
    }
    return output;
}

support::ExpectedVoid OverlayCompositor::composite(
    TerminalDimensions dimensions,
    const TerminalCapabilities& capabilities,
    RenderResult& output) const {
    auto visible_overlays = sorted_visible(dimensions);

    for (auto* overlay : visible_overlays) {
        auto rendered = overlay->render(dimensions.columns);
        if (!rendered) return std::unexpected(rendered.error());
        const auto& constraints = overlay->options().size_constraints;
        const auto max_height = constraints.max_height.value_or(dimensions.rows);
        const auto materialization_width = std::min(
            dimensions.columns,
            constraints.max_width.value_or(dimensions.columns));
        auto materialized = materialize_images(
            std::move(*rendered),
            capabilities,
            materialization_width,
            std::min(dimensions.rows, max_height));
        if (materialized.lines.empty()) continue;

        std::size_t content_width = 0;
        for (const auto& line : materialized.lines) {
            content_width = std::max(content_width, visible_width(line));
        }
        const auto content_height = materialized.lines.size();
        const auto [offset_col, offset_row] = overlay->layout_position(
            dimensions.columns, dimensions.rows, content_width, content_height);
        const auto& margins = overlay->options().margins;
        const auto final_col = offset_col + margins.left;
        const auto final_row = offset_row + margins.top;
        if (final_col >= dimensions.columns || final_row >= dimensions.rows) continue;

        // The composed buffer may exceed the viewport under the main-screen
        // scrollback flow; overlays are viewport-positioned, so their rows are
        // offset by the current viewport start (pi compositeOverlays
        // `viewportStart = max(0, workingHeight - termHeight)`).
        const auto viewport_start = output.lines.size() > dimensions.rows
            ? output.lines.size() - dimensions.rows
            : 0;

        const auto overlay_columns = dimensions.columns - final_col;
        const auto overlay_rows = dimensions.rows - final_row;
        std::erase_if(materialized.images, [&](const auto& image) {
            const auto fits_columns = image.region.column < overlay_columns &&
                image.region.columns <= overlay_columns - image.region.column;
            const auto fits_rows = image.region.row < overlay_rows &&
                image.region.rows <= overlay_rows - image.region.row;
            if (fits_columns && fits_rows) return false;
            if (image.region.row < materialized.lines.size()) {
                materialized.lines[image.region.row] = image.fallback_text;
            }
            return true;
        });

        std::size_t composited_width = 0;
        for (const auto& line : materialized.lines) {
            composited_width = std::max(composited_width, visible_width(line));
        }
        const auto overlay_width = std::min(composited_width, dimensions.columns - final_col);
        for (std::size_t row = 0; row < content_height; ++row) {
            if (final_row + row >= dimensions.rows || overlay_width == 0) break;
            const auto target_row = viewport_start + final_row + row;
            if (target_row >= output.lines.size()) output.lines.resize(target_row + 1);
            auto composited = replace_line_region(
                output.lines[target_row],
                materialized.lines[row],
                final_col,
                overlay_width,
                dimensions.columns);
            if (!composited) return std::unexpected(composited.error());
            output.lines[target_row] = std::move(*composited);

            const CellRegion written{
                .column = final_col,
                .row = target_row,
                .columns = overlay_width,
                .rows = 1,
            };
            std::erase_if(output.images, [&](const auto& image) {
                return cell_regions_intersect(image.region, written);
            });
        }

        for (auto& image : materialized.images) {
            image.region.column += final_col;
            image.region.row += viewport_start + final_row;
            const auto fits_columns = image.region.column < dimensions.columns &&
                image.region.columns <= dimensions.columns - image.region.column;
            const auto viewport_row = image.region.row - viewport_start;
            const auto fits_rows = viewport_row < dimensions.rows &&
                image.region.rows <= dimensions.rows - viewport_row;
            if (fits_columns && fits_rows) output.images.push_back(std::move(image));
        }
    }

    return {};
}

bool OverlayCompositor::dispatch_input(const InputEventVariant& event, TerminalDimensions viewport) const {
    // Try overlays first (in reverse z-order, topmost first)
    auto visible = sorted_visible(viewport);
    for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
        auto* overlay = *it;
        // Non-capturing overlays pass input through
        if (overlay->options().non_capturing) continue;

        if (auto* input_handler = dynamic_cast<InputHandler*>(overlay)) {
            if (const auto* key = std::get_if<KeyEvent>(&event);
                key != nullptr && key->type == KeyEventType::Release && !input_handler->accepts_key_releases()) {
                continue;
            }
            input_handler->handle_input(event);
            return true;
        }
    }
    return false;
}

Overlay* OverlayCompositor::topmost_focusable(TerminalDimensions viewport) const {
    auto visible = sorted_visible(viewport);
    for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
        auto* focusable = dynamic_cast<Focusable*>(*it);
        if (focusable != nullptr && !(*it)->options().non_capturing) return *it;
    }
    return nullptr;
}

bool OverlayCompositor::focus_target_available(Component* component, TerminalDimensions viewport) const {
    auto* overlay = dynamic_cast<Overlay*>(component);
    return overlay != nullptr && owns(overlay) && overlay->visible_at(viewport) &&
        !overlay->options().non_capturing;
}

void OverlayCompositor::remember_focus(Overlay* overlay, Component* previous) {
    const auto found = std::find_if(
        focus_history_.begin(),
        focus_history_.end(),
        [overlay](const auto& entry) { return entry.overlay == overlay; });
    if (found == focus_history_.end()) {
        focus_history_.push_back({.overlay = overlay, .previous = previous});
    } else {
        found->previous = previous;
    }
}

Component* OverlayCompositor::return_focus(const Overlay* overlay) const {
    const auto found = std::find_if(
        focus_history_.begin(),
        focus_history_.end(),
        [overlay](const auto& entry) { return entry.overlay == overlay; });
    return found == focus_history_.end() ? nullptr : found->previous;
}

void OverlayCompositor::forget_focus(Overlay* overlay, Component* replacement) {
    for (auto& entry : focus_history_) {
        if (entry.previous == static_cast<Component*>(overlay)) entry.previous = replacement;
    }
    std::erase_if(focus_history_, [overlay](const auto& entry) {
        return entry.overlay == overlay;
    });
}

void OverlayCompositor::clear_focus_history() {
    focus_history_.clear();
}

std::vector<Overlay*> OverlayCompositor::sorted_visible(TerminalDimensions viewport) const {
    std::vector<Overlay*> visible;
    for (const auto& overlay : overlays_) {
        if (overlay->visible_at(viewport)) visible.push_back(overlay.get());
    }
    std::sort(visible.begin(), visible.end(), [](const Overlay* a, const Overlay* b) {
        return a->options().z_index < b->options().z_index;
    });
    return visible;
}

} // namespace cch::tui::detail
