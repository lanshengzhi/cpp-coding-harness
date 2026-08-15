#include <cch/tui/Tui.hpp>

#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/src/tui.ts and
// packages/tui/src/components/image.ts, sidecar fork B. Placement is
// terminal-owned: Tui materializes InlineImageRenderRegions with the
// per-render capabilities (protocol + cell size, pi's 9x18 default refined
// by CSI 16 t responses), reuses the placement identity (resource_id + cell
// region) across animation frames by re-placing with the same protocol image
// id (pi's imageId reuse) so Kitty updates frames in place, and removes stale
// placements through the terminal seam without pi's in-line sequence
// bookkeeping.

namespace cch::tui {
namespace {

constexpr std::size_t kInputDecodeChunkBytes = 4096;

[[nodiscard]] std::string describe_error(const support::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] support::Error startup_rollback_error(
    const support::Error& startup,
    const support::Error& rollback) {
    return support::make_error(
        startup.code,
        "TUI startup failed and terminal restoration was incomplete",
        std::format(
            "startup: {}; restoration: {}",
            describe_error(startup),
            describe_error(rollback)));
}

/// Write a single full-width line to the terminal at the given row.
[[nodiscard]] support::ExpectedVoid write_line(
    Terminal& terminal,
    std::size_t row,
    std::string_view line) {
    if (auto result = terminal.set_cursor(CursorPosition{.column = 0, .row = row}); !result) {
        return std::unexpected(result.error());
    }
    return terminal.write(line);
}

/// Clear a row of the terminal by writing spaces at the full terminal width.
[[nodiscard]] support::ExpectedVoid clear_row(Terminal& terminal, std::size_t row, std::size_t columns) {
    if (auto result = terminal.set_cursor(CursorPosition{.column = 0, .row = row}); !result) {
        return std::unexpected(result.error());
    }
    // Write spaces to fill the row, then move cursor back so the next write overwrites cleanly
    return terminal.write(std::string(columns, ' '));
}

[[nodiscard]] support::Expected<std::string> line_suffix_from_column(
    std::string_view line,
    std::size_t column) {
    auto tokens = detail::tokenize_terminal_output(line);
    if (!tokens) return std::unexpected(tokens.error());

    detail::AnsiStyleState style;
    std::string suffix;
    std::size_t visible_column = 0;
    bool suffix_started = false;
    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
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

/// Sort overlays by z_index ascending for rendering draw order.
[[nodiscard]] std::vector<Overlay*> sorted_visible_overlays(
    std::vector<std::unique_ptr<Overlay>>& overlays,
    TerminalDimensions viewport) {
    std::vector<Overlay*> visible;
    for (const auto& overlay : overlays) {
        if (overlay->visible_at(viewport)) visible.push_back(overlay.get());
    }
    std::sort(visible.begin(), visible.end(), [](const Overlay* a, const Overlay* b) {
        return a->options().z_index < b->options().z_index;
    });
    return visible;
}

} // namespace

Tui::Tui(Terminal& terminal)
    : terminal_(terminal),
      input_decoder_(std::make_unique<detail::InputDecoder>()) {}

Tui::~Tui() {
    (void)stop();
}

support::Expected<std::reference_wrapper<Component>> Tui::add_child(std::unique_ptr<Component> component) {
    std::lock_guard lock(mutex_);
    return detail::attach_child(children_, std::move(component), "");
}

support::Expected<std::reference_wrapper<Overlay>> Tui::add_overlay(std::unique_ptr<Overlay> overlay) {
    std::lock_guard lock(mutex_);
    if (!overlay) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI cannot attach a null Overlay"));
    }
    auto& ref = *overlay;
    overlays_.push_back(std::move(overlay));
    return ref;
}

support::ExpectedVoid Tui::remove_overlay(Overlay* overlay) {
    std::lock_guard lock(mutex_);
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    const bool was_focused = focused_ == static_cast<Component*>(overlay);
    auto* return_focus = overlay_return_focus(overlay);
    if (was_focused) apply_focus(nullptr);
    forget_overlay_focus(overlay, return_focus);

    const auto before = overlays_.size();
    std::erase_if(overlays_, [overlay](const auto& ptr) { return ptr.get() == overlay; });
    if (overlays_.size() == before) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay not found in TUI"));
    }

    if (was_focused) {
        if (focus_target_available(return_focus)) apply_focus(return_focus);
        else fallback_focus();
    }
    return {};
}

support::ExpectedVoid Tui::hide_overlay(Overlay* overlay) {
    std::lock_guard lock(mutex_);
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    auto* return_focus = overlay_return_focus(overlay);
    const auto was_focused = focused_ == static_cast<Component*>(overlay);
    overlay->set_visible(false);

    if (was_focused) {
        apply_focus(nullptr);
        if (focus_target_available(return_focus)) apply_focus(return_focus);
        else fallback_focus();
    }

    invalidate();
    return {};
}

support::ExpectedVoid Tui::restore_overlay(Overlay* overlay) {
    std::lock_guard lock(mutex_);
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    overlay->set_visible(true);
    invalidate();
    return {};
}

support::ExpectedVoid Tui::start() {
    std::unique_lock lock(mutex_);
    if (started_) {
        return {};
    }

    if (auto result = terminal_.start(
            [this](std::string input) { handle_input(std::move(input)); },
            [this](TerminalDimensions dimensions) { handle_resize(dimensions); });
        !result) {
        return std::unexpected(result.error());
    }
    started_ = true;

    if (auto result = terminal_.set_cursor_visible(false); !result) {
        started_ = false;
        lock.unlock();
        if (auto stopped = terminal_.stop(); !stopped) {
            return std::unexpected(startup_rollback_error(result.error(), stopped.error()));
        }
        return std::unexpected(result.error());
    }

    first_render_ = true;
    pending_render_ = false;
    viewport_top_ = 0;
    previous_lines_.clear();
    previous_dimensions_ = terminal_.dimensions();
    return {};
}

support::ExpectedVoid Tui::stop() {
    std::unique_lock lock(mutex_);
    if (!started_) return {};

    started_ = false;
    if (auto* focusable = dynamic_cast<Focusable*>(focused_)) {
        focusable->set_focused(false);
    }
    focused_ = nullptr;
    overlay_focus_history_.clear();

    const auto image_result = remove_active_images();
    const auto cursor_result = terminal_.set_cursor_visible(true);
    active_images_.clear();
    input_decoder_->reset();
    first_render_ = true;
    pending_render_ = false;

    // Let an already-delivered Process Terminal callback observe stopped state
    // before the terminal joins its delivery worker.
    lock.unlock();
    const auto stop_result = terminal_.stop();

    if (!image_result) return std::unexpected(image_result.error());
    if (!cursor_result) return std::unexpected(cursor_result.error());
    if (!stop_result) return std::unexpected(stop_result.error());
    return {};
}

support::ExpectedVoid Tui::clear_screen() {
    std::lock_guard lock(mutex_);
    if (!started_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI must be started before clearing the screen"));
    }
    if (auto removed = remove_active_images(); !removed) {
        return std::unexpected(removed.error());
    }
    if (auto cleared = terminal_.clear_screen(); !cleared) {
        return std::unexpected(cleared.error());
    }
    previous_lines_.clear();
    previous_dimensions_ = {};
    viewport_top_ = 0;
    first_render_ = true;
    return {};
}

support::ExpectedVoid Tui::render() {
    std::lock_guard lock(mutex_);
    if (!started_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI must be started before rendering"));
    }

    const auto dimensions = terminal_.dimensions();
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI requires positive terminal dimensions"));
    }

    const auto capabilities = terminal_.capabilities();
    auto rendered = render_children(dimensions);
    if (!rendered) return std::unexpected(rendered.error());
    // Main-screen images are buffer-absolute and follow content into the
    // terminal's scrollback (fork-B image-follows-content): they are not
    // bounded by the viewport, only by the composed buffer itself.
    auto materialized = materialize_images(
        std::move(*rendered),
        capabilities,
        dimensions.columns,
        std::numeric_limits<std::size_t>::max());
    if (auto overlay_result = composite_overlays(dimensions, capabilities, materialized);
        !overlay_result) {
        return std::unexpected(overlay_result.error());
    }
    auto& new_lines = materialized.lines;
    auto desired_images = std::move(materialized.images);

    // The full composed buffer is written to the terminal's main screen with
    // no viewport clipping; overflow advances into the terminal's native
    // scrollback (pi TuiMainScreen). Lines are padded so the first-diff
    // comparison is byte-stable across renders.
    for (auto& line : new_lines) {
        if (line.size() < dimensions.columns) {
            line.append(dimensions.columns - line.size(), ' ');
        }
    }

    const auto width_changed = dimensions.columns != previous_dimensions_.columns;
    const auto height_changed = dimensions.rows != previous_dimensions_.rows;
    const auto first_render = first_render_;

    const auto supports_sync = capabilities.synchronized_output;

    // Begin synchronized update if supported
    if (supports_sync) {
        if (auto result = terminal_.begin_synchronized_update(); !result) {
            return std::unexpected(result.error());
        }
    }

    // Write the full composed buffer with pi's line-flow ordering (rows at or
    // past the visible bottom advance the terminal's scrollback through the
    // absolute-cursor seam).
    auto write_full_buffer = [&]() -> support::ExpectedVoid {
        for (std::size_t row = 0; row < new_lines.size(); ++row) {
            if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                return std::unexpected(result.error());
            }
        }
        return {};
    };

    // pi fullRender(true): drop all image placements, clear screen, home, and
    // clear scrollback (`\x1b[2J\x1b[H\x1b[3J`), then reflow the full buffer so
    // the terminal's scroll history starts clean.
    auto clear_and_rewrite = [&]() -> support::ExpectedVoid {
        if (auto result = remove_active_images(); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = terminal_.clear_screen(); !result) {
            return std::unexpected(result.error());
        }
        viewport_top_ = 0;
        return write_full_buffer();
    };

    auto render_result = [&]() -> support::ExpectedVoid {
        if (first_render) {
            // pi fullRender(false): write the full buffer without clearing
            // ("assumes clean screen"), so startup content stays visible until
            // the buffer grows past one screen and scrolls away.
            if (auto result = write_full_buffer(); !result) {
                return std::unexpected(result.error());
            }
            // Leave the cursor at the end of the written content (column 0) so
            // the terminal advances its scrollback past the rendered content.
            if (auto result = terminal_.set_cursor(CursorPosition{
                    .column = 0,
                    .row = new_lines.empty() ? 0U : new_lines.size() - 1});
                !result) {
                return std::unexpected(result.error());
            }
            return {};
        }

        // A width or height change reflows from a clean screen — clear screen,
        // home, clear scrollback — so reflow starts clean and the terminal's
        // scroll history is cleared, matching pi (the Termux height-change
        // special-case is not applicable and is not ported).
        if (width_changed || height_changed) {
            return clear_and_rewrite();
        }

        // pi differential: first-changed-line tracking over the full buffer.
        const auto min_previous = std::min(previous_lines_.size(), new_lines.size());
        std::size_t first_diff = 0;
        while (first_diff < min_previous && previous_lines_[first_diff] == new_lines[first_diff]) {
            ++first_diff;
        }
        const auto unchanged =
            first_diff == min_previous && previous_lines_.size() == new_lines.size();
        if (unchanged) return {};

        // A change above the tracked viewport, or new content that ends above
        // it, cannot be reached with line flow: reflow from a clean screen
        // (pi `firstChanged < viewportTop` / `targetRow < viewportTop` full
        // redraw).
        const auto target_row = new_lines.empty() ? 0U : new_lines.size() - 1;
        if (first_diff < viewport_top_ || target_row < viewport_top_) {
            return clear_and_rewrite();
        }

        // Line-flow differential: write changed and appended lines from
        // first_diff through the end of the buffer. Rows at or past the
        // visible bottom advance the terminal's scrollback (the absolute-
        // cursor seam scrolls on addressing a row below the viewport).
        for (std::size_t row = first_diff; row < new_lines.size(); ++row) {
            const CellRegion region{
                .column = 0,
                .row = row,
                .columns = dimensions.columns,
                .rows = 1,
            };
            if (auto result = remove_images_intersecting(region); !result) {
                return std::unexpected(result.error());
            }
            if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                return std::unexpected(result.error());
            }
        }

        // Clear-on-shrink: stale rows below the new content that are still
        // inside the visible viewport are cleared in place (rows that already
        // scrolled into the terminal's scrollback keep their history).
        if (new_lines.size() < previous_lines_.size()) {
            const auto stale_end = std::min(
                previous_lines_.size(),
                viewport_top_ + dimensions.rows);
            for (std::size_t row = new_lines.size(); row < stale_end; ++row) {
                const CellRegion region{
                    .column = 0,
                    .row = row,
                    .columns = dimensions.columns,
                    .rows = 1,
                };
                if (auto result = remove_images_intersecting(region); !result) {
                    return std::unexpected(result.error());
                }
                if (auto result = clear_row(terminal_, row, dimensions.columns); !result) {
                    return std::unexpected(result.error());
                }
            }
        }
        return {};
    }();

    if (render_result) {
        if (auto stale_result = remove_stale_images(desired_images); !stale_result) {
            render_result = std::unexpected(stale_result.error());
        }
    }

    if (render_result) {
        if (auto image_result = place_images(desired_images); !image_result) {
            render_result = std::unexpected(image_result.error());
        }
    }

    // Track the viewport top over the buffer before positioning the hardware
    // cursor: after writing the full buffer (or diffing to its end) the
    // terminal shows the bottom `rows` lines of the composed buffer (pi
    // `previousViewportTop = max(prev, len - height)`). The stale-row clearing
    // inside the render body used the pre-write viewport, which is the correct
    // bound for rows already visible before any scroll.
    viewport_top_ = std::max(
        viewport_top_,
        new_lines.size() > dimensions.rows ? new_lines.size() - dimensions.rows : 0U);

    // Position IME cursor based on focused component
    if (render_result) {
        auto cursor_loc = resolve_cursor_location();
        if (cursor_loc) {
            // Clamp the buffer-relative cursor to the visible viewport so the
            // hardware cursor stays on screen (pi positionHardwareCursor); the
            // cursor must not scroll the terminal, only track it.
            const auto viewport_bottom = viewport_top_ + dimensions.rows - 1;
            if (cursor_loc->row < viewport_top_) cursor_loc->row = viewport_top_;
            else if (cursor_loc->row > viewport_bottom) cursor_loc->row = viewport_bottom;
            // Position the hardware cursor at the focused component's cursor location
            // for IME composition without making it visible by default
            // We position but keep invisible so the IME knows where the cursor is
            if (auto cursor_result = terminal_.set_cursor(*cursor_loc); !cursor_result) {
                render_result = std::unexpected(cursor_result.error());
            }
        }
    }

    if (supports_sync) {
        if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
            if (!render_result) return std::unexpected(render_result.error());
            return std::unexpected(end_result.error());
        }
    }

    if (!render_result) return std::unexpected(render_result.error());

    // Update cached state
    previous_lines_ = std::move(new_lines);
    previous_dimensions_ = dimensions;
    first_render_ = false;
    pending_render_ = false;
    return {};
}

support::Expected<RenderResult> Tui::render_children(TerminalDimensions dimensions) {
    RenderResult output;
    for (const auto& child : children_) {
        if (auto* viewport_aware = dynamic_cast<ViewportAware*>(child.get())) {
            viewport_aware->set_available_height(dimensions.rows);
        }
        auto rendered = child->render(dimensions.columns);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = output.lines.size();
        for (auto& line : rendered->lines) {
            auto prepared = detail::prepare_rendered_line(line, dimensions.columns);
            if (!prepared) return std::unexpected(prepared.error());
            output.lines.push_back(std::move(*prepared));
        }
        for (auto& image : rendered->images) {
            image.region.row += row_offset;
            output.images.push_back(std::move(image));
        }
    }
    return output;
}

RenderResult Tui::materialize_images(
    RenderResult output,
    const TerminalCapabilities& capabilities,
    std::size_t width,
    std::size_t available_rows) const {
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
        if (!detail::protocol_supports_mime(capabilities.inline_images, image.mime_type) ||
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

support::ExpectedVoid Tui::composite_overlays(
    TerminalDimensions dimensions,
    const TerminalCapabilities& capabilities,
    RenderResult& output) {
    auto visible_overlays = sorted_visible_overlays(overlays_, dimensions);

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
                return detail::cell_regions_intersect(image.region, written);
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

support::ExpectedVoid Tui::remove_active_images() {
    while (!active_images_.empty()) {
        const auto image = active_images_.back();
        if (auto removed = terminal_.remove_image(image.handle, image.region); !removed) {
            return std::unexpected(removed.error());
        }
        active_images_.pop_back();
    }
    return {};
}

support::ExpectedVoid Tui::remove_images_intersecting(const CellRegion& region) {
    std::size_t index = 0;
    while (index < active_images_.size()) {
        const auto& image = active_images_[index];
        if (!detail::cell_regions_intersect(image.region, region)) {
            ++index;
            continue;
        }
        if (auto removed = terminal_.remove_image(image.handle, image.region); !removed) {
            return std::unexpected(removed.error());
        }
        active_images_.erase(active_images_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return {};
}

support::ExpectedVoid Tui::remove_stale_images(
    const std::vector<InlineImageRenderRegion>& desired_images) {
    std::size_t index = 0;
    while (index < active_images_.size()) {
        const auto& active = active_images_[index];
        // Placement identity is (resource_id, region): a revision bump is an
        // animation frame that place_images() re-places in place (reusing the
        // protocol image id), never a removal.
        const auto retained = std::find_if(
            desired_images.begin(),
            desired_images.end(),
            [&](const auto& desired) {
                return active.resource_id == desired.resource_id &&
                    active.region == desired.region;
            });
        if (retained != desired_images.end()) {
            ++index;
            continue;
        }
        if (auto removed = terminal_.remove_image(active.handle, active.region); !removed) {
            return std::unexpected(removed.error());
        }
        active_images_.erase(active_images_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return {};
}

support::ExpectedVoid Tui::place_images(
    const std::vector<InlineImageRenderRegion>& desired_images) {
    for (const auto& image : desired_images) {
        const auto active = std::find_if(
            active_images_.begin(),
            active_images_.end(),
            [&](const auto& candidate) {
                return candidate.resource_id == image.resource_id &&
                    candidate.region == image.region;
            });
        if (active != active_images_.end() && active->revision == image.revision) {
            continue;
        }

        std::optional<std::string_view> filename;
        if (image.filename) filename = *image.filename;
        TerminalImage terminal_image{
            .encoded_data = image.encoded_data,
            .mime_type = image.mime_type,
            .filename = filename,
            .pixel_width = image.pixel_width,
            .pixel_height = image.pixel_height,
            .resource_id = image.resource_id,
            .revision = image.revision,
            .region = image.region,
        };
        if (active != active_images_.end()) {
            // Animation frame: re-place with the same protocol image id so
            // Kitty replaces the image in place (pi's imageId reuse) instead
            // of delete-plus-recreate.
            terminal_image.preferred_handle = active->handle;
        }
        auto placed = terminal_.place_image(terminal_image);
        if (!placed) return std::unexpected(placed.error());
        if (active != active_images_.end()) {
            active->revision = image.revision;
            continue;
        }
        active_images_.push_back({
            .handle = *placed,
            .region = image.region,
            .resource_id = image.resource_id,
            .revision = image.revision,
        });
    }
    return {};
}

support::ExpectedVoid Tui::set_focus(Component* component) {
    std::lock_guard lock(mutex_);
    if (component != nullptr && !owns(component)) {
        // Check if component is inside an overlay
        bool found_in_overlay = false;
        for (const auto& overlay : overlays_) {
            if (static_cast<Component*>(overlay.get()) == component) {
                found_in_overlay = true;
                break;
            }
        }
        if (!found_in_overlay) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "TUI focus target is not attached to this root"));
        }
    }
    if (component != nullptr && dynamic_cast<Focusable*>(component) == nullptr) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI focus target does not participate in focus"));
    }

    if (auto* overlay = dynamic_cast<Overlay*>(component); overlay != nullptr && component != focused_) {
        remember_overlay_focus(overlay);
    }
    apply_focus(component);
    return {};
}

void Tui::set_render_request_sink(TuiRenderRequestSink sink) {
    std::lock_guard lock(mutex_);
    render_request_sink_ = std::move(sink);
}

void Tui::invalidate() {
    std::lock_guard lock(mutex_);
    const bool request_render = started_ && !pending_render_;
    pending_render_ = true;
    for (const auto& child : children_) {
        child->invalidate();
    }
    for (const auto& overlay : overlays_) {
        overlay->invalidate();
    }
    if (request_render && render_request_sink_) {
        try {
            render_request_sink_();
        } catch (...) {
            // Scheduling notifications cannot make terminal input delivery fail.
        }
    }
}

bool Tui::owns(const Component* component) const {
    for (const auto& child : children_) {
        if (child.get() == component) {
            return true;
        }
    }
    return false;
}

bool Tui::owns_overlay(const Overlay* overlay) const {
    for (const auto& owned : overlays_) {
        if (owned.get() == overlay) return true;
    }
    return false;
}

void Tui::handle_input(std::string input) {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    if (input.empty()) {
        for (const auto& event : input_decoder_->flush()) dispatch_input(event);
        return;
    }

    for (std::size_t offset = 0; offset < input.size(); offset += kInputDecodeChunkBytes) {
        const auto chunk = std::string_view(input).substr(offset, kInputDecodeChunkBytes);
        for (const auto& event : input_decoder_->feed(chunk)) dispatch_input(event);
    }
}

void Tui::dispatch_input(const InputEventVariant& event) {
    // Try overlays first (in reverse z-order, topmost first)
    auto visible = sorted_visible_overlays(overlays_, previous_dimensions_);
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
            return;
        }
    }

    // Fallback to focused base component
    auto* input_handler = dynamic_cast<InputHandler*>(focused_);
    if (input_handler == nullptr) {
        // Try first focusable for initial dispatch
        fallback_focus();
        input_handler = dynamic_cast<InputHandler*>(focused_);
        if (input_handler == nullptr) return;
    }
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key != nullptr && key->type == KeyEventType::Release && !input_handler->accepts_key_releases()) {
        return;
    }
    input_handler->handle_input(event);
}

void Tui::handle_resize(TerminalDimensions) {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    invalidate();
}

void Tui::apply_focus(Component* component) {
    if (auto* previous = dynamic_cast<Focusable*>(focused_)) previous->set_focused(false);
    focused_ = component;
    if (auto* next = dynamic_cast<Focusable*>(focused_)) next->set_focused(true);
}

void Tui::remember_overlay_focus(Overlay* overlay) {
    const auto found = std::find_if(
        overlay_focus_history_.begin(),
        overlay_focus_history_.end(),
        [overlay](const auto& entry) { return entry.overlay == overlay; });
    if (found == overlay_focus_history_.end()) {
        overlay_focus_history_.push_back({.overlay = overlay, .previous = focused_});
    } else {
        found->previous = focused_;
    }
}

Component* Tui::overlay_return_focus(const Overlay* overlay) const {
    const auto found = std::find_if(
        overlay_focus_history_.begin(),
        overlay_focus_history_.end(),
        [overlay](const auto& entry) { return entry.overlay == overlay; });
    return found == overlay_focus_history_.end() ? nullptr : found->previous;
}

void Tui::forget_overlay_focus(Overlay* overlay, Component* replacement) {
    for (auto& entry : overlay_focus_history_) {
        if (entry.previous == static_cast<Component*>(overlay)) entry.previous = replacement;
    }
    std::erase_if(overlay_focus_history_, [overlay](const auto& entry) {
        return entry.overlay == overlay;
    });
}

bool Tui::focus_target_available(Component* component) const {
    if (component == nullptr) return false;
    if (owns(component)) return dynamic_cast<Focusable*>(component) != nullptr;
    auto* overlay = dynamic_cast<Overlay*>(component);
    return overlay != nullptr && owns_overlay(overlay) &&
        overlay->visible_at(previous_dimensions_) && !overlay->options().non_capturing;
}

void Tui::fallback_focus() {
    if (auto* target = find_focusable_target()) {
        auto* as_component = dynamic_cast<Component*>(target);
        if (as_component != focused_) apply_focus(as_component);
    }
}

Focusable* Tui::find_focusable_target() {
    // Look in visible overlays first (topmost first)
    auto visible = sorted_visible_overlays(overlays_, previous_dimensions_);
    for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
        auto* focusable = dynamic_cast<Focusable*>(*it);
        if (focusable != nullptr && !(*it)->options().non_capturing) return focusable;
    }

    // Fallback to first focusable base child
    for (const auto& child : children_) {
        auto* focusable = dynamic_cast<Focusable*>(child.get());
        if (focusable != nullptr) return focusable;
    }
    return nullptr;
}

std::optional<CursorPosition> Tui::resolve_cursor_location() const {
    if (focused_ == nullptr) return std::nullopt;
    auto* focusable = dynamic_cast<Focusable*>(focused_);
    if (focusable == nullptr) return std::nullopt;
    if (!focusable->focused()) return std::nullopt;
    return focusable->cursor_location();
}

} // namespace cch::tui
