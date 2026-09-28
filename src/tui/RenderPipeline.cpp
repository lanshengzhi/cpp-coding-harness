#include "RenderPipeline.hpp"

#include <cch/tui/TerminalImage.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/src/tui.ts and
// packages/tui/src/components/image.ts, sidecar fork B. Placement is
// terminal-owned: Tui::render materializes InlineImageRenderRegions with the
// per-render capabilities (protocol + cell size, pi's 9x18 default refined
// by CSI 16 t responses); this module reuses the placement identity
// (resource_id + cell region) across animation frames by re-placing with the
// same protocol image id (pi's imageId reuse) so Kitty updates frames in
// place, and removes stale placements through the terminal seam without pi's
// in-line sequence bookkeeping.

namespace cch::tui::detail {
namespace {

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

/// Write a single full-width line directly to the terminal's reserved bottom dock region.
[[nodiscard]] support::ExpectedVoid write_dock_line(Terminal& terminal, std::size_t dock_row, std::string_view line) {
    if (auto result = terminal.set_dock_cursor(dock_row, 0); !result) {
        return std::unexpected(result.error());
    }
    return terminal.write(line);
}


} // namespace

RenderPipeline::RenderPipeline(Terminal& terminal) : terminal_(terminal) {}

void RenderPipeline::start(TerminalDimensions dimensions) noexcept {
    first_render_ = true;
    viewport_top_ = 0;
    previous_lines_.clear();
    previous_viewport_height_ = 0;
    previous_dimensions_ = dimensions;
    admitted_ = {};
}

void RenderPipeline::begin_frame() noexcept { frame_prepare_call_count_ = 0; }

void RenderPipeline::note_prepared_line() noexcept { ++frame_prepare_call_count_; }

std::size_t RenderPipeline::frame_prepare_call_count() const noexcept { return frame_prepare_call_count_; }

support::ExpectedVoid RenderPipeline::stop(std::optional<CursorPosition> stop_cursor) {
    const auto image_result = remove_active_images();
    (void)terminal_.reset_scroll_margins();
    const auto dock_height = previous_dock_lines_.size();
    const auto dimensions = terminal_.dimensions();
    const auto dock_capacity =
            dimensions.rows > previous_viewport_height_ ? dimensions.rows - previous_viewport_height_ : std::size_t{0};
    const auto visible_dock_height = std::min(dock_height, dock_capacity);
    const auto dock_skip = dock_height - visible_dock_height;
    previous_dock_lines_.clear();

    support::ExpectedVoid exit_result;
    if (!previous_lines_.empty()) {
        if (auto written = terminal_.write(" "); !written) {
            exit_result = std::unexpected(written.error());
        } else if (dock_height == 0) {
            if (auto positioned = terminal_.set_cursor(CursorPosition{.column = 0, .row = previous_lines_.size()});
                    !positioned) {
                exit_result = std::unexpected(positioned.error());
            } else if (auto newline = terminal_.write("\r\n"); !newline) {
                exit_result = std::unexpected(newline.error());
            }
        } else {
            std::size_t current_row = dimensions.rows - 1;
            if (visible_dock_height > 0 && stop_cursor) {
                std::size_t dock_row = stop_cursor->row;
                if (dock_row >= previous_viewport_height_ && dock_row < dimensions.rows) {
                    dock_row -= previous_viewport_height_;
                }
                if (dock_row < dock_height) {
                    const auto visible_row = dock_row > dock_skip ? dock_row - dock_skip : 0;
                    current_row =
                            dimensions.rows - visible_dock_height + std::min(visible_row, visible_dock_height - 1);
                } else {
                    current_row = std::min(stop_cursor->row, dimensions.rows - 1);
                }
            }
            auto rows_down = current_row < dimensions.rows - 1 ? dimensions.rows - 1 - current_row : std::size_t{0};
            if (stop_cursor && stop_cursor->column >= dimensions.columns && rows_down > 0) --rows_down;
            if (rows_down > 0) {
                exit_result = terminal_.move_cursor_down(rows_down);
            }
            if (exit_result) exit_result = terminal_.write("\r\n");
        }
    }

    active_images_.clear();
    first_render_ = true;
    if (!image_result) return std::unexpected(image_result.error());
    return exit_result;
}

support::ExpectedVoid RenderPipeline::clear_screen() {
    if (auto removed = remove_active_images(); !removed) {
        return std::unexpected(removed.error());
    }
    if (auto cleared = terminal_.clear_screen(); !cleared) {
        return std::unexpected(cleared.error());
    }
    (void)terminal_.reset_scroll_margins();
    previous_dock_lines_.clear();
    previous_lines_.clear();
    previous_raw_dock_lines_.clear();
    previous_raw_lines_.clear();
    previous_viewport_height_ = 0;
    previous_dimensions_ = {};
    viewport_top_ = 0;
    admitted_ = {};
    first_render_ = true;
    return {};
}

support::ExpectedVoid RenderPipeline::render(RenderResult materialized,
        TerminalDimensions dimensions,
        TerminalCapabilities capabilities,
        bool compose_overlays,
        std::optional<CursorPosition> cursor_location) {
    auto& new_lines = materialized.lines;
    auto desired_images = std::move(materialized.images);
    auto& new_dock_lines = materialized.dock_lines;
    const bool has_dock = !new_dock_lines.empty() || materialized.viewport_height.has_value();
    const std::size_t dock_height = new_dock_lines.size();
    const std::size_t viewport_height =
            materialized.viewport_height.value_or(dimensions.rows > dock_height ? dimensions.rows - dock_height : 0);
    // A dock taller than its addressable rows (an oversized replacement
    // dialog or autocomplete under a shrunk terminal, #607) is cropped from
    // the top so the editor and footer keep the physical bottom rows.
    // Cropped rows are never addressed, so the overflow is nonfatal; real
    // terminal write failures still propagate.
    const std::size_t dock_capacity =
            has_dock && viewport_height >= 1 ? dimensions.rows - viewport_height : dimensions.rows;
    const std::size_t dock_skip = dock_height > dock_capacity ? dock_height - dock_capacity : 0;

    const auto width_changed = dimensions.columns != previous_dimensions_.columns;
    const auto height_changed = dimensions.rows != previous_dimensions_.rows;
    // A viewport/dock re-partition (overlay, autocomplete, editor wrap) moves
    // physical rows between viewport and dock: repaint the full buffer at the
    // new partition so orphaned rows rejoin with buffer content (#597).
    const auto viewport_height_changed = !first_render_ && viewport_height != previous_viewport_height_;
    const auto first_render = first_render_;

    // Padding and the one full reset a composed row carries (pi
    // `applyLineResets` after `render()` and after `compositeOverlays()`, so a
    // reset can never land inside a background span, #707). Padding keeps the
    // first-diff comparison byte-stable across renders.
    // ANSI bytes do not occupy terminal cells; pad by visible columns so a
    // shorter replacement cannot leave the previous row's suffix visible.
    const auto pad_and_reset_rows = [&](std::vector<std::string>& lines) {
        for (auto& line : lines) {
            const auto visible_columns = detail::measure_visible_width(line).width;
            if (visible_columns < dimensions.columns) {
                line.append(dimensions.columns - visible_columns, ' ');
            }
            detail::apply_line_reset(line);
        }
    };

    std::vector<std::string> next_raw_lines;
    std::vector<std::string> next_raw_dock_lines;
    if (compose_overlays) {
        // Preparation already ran before compositing: an overlay splices into
        // prepared composed rows, so that frame keeps the original order.
        pad_and_reset_rows(new_lines);
        pad_and_reset_rows(new_dock_lines);
    } else {
        // Frame-level preparation follows the differential state: a composed row
        // is normalized, checked against the width bound, padded to the terminal
        // width, and given its single reset — but only when its composed bytes
        // changed. A Preview Frame that changes nothing prepares nothing (#711).
        const bool rows_reusable = !first_render && !width_changed;
        const auto prepare_frame_rows = [&](std::vector<std::string>& lines,
                                                const std::vector<std::string>& previous_prepared,
                                                const std::vector<std::string>& previous_raw,
                                                std::vector<std::string>& next_raw) -> support::ExpectedVoid {
            next_raw.resize(lines.size());
            for (std::size_t index = 0; index < lines.size(); ++index) {
                const bool reusable = rows_reusable && index < previous_prepared.size() &&
                                      index < previous_raw.size() && lines[index] == previous_raw[index];
                // The composed bytes move into the raw cache either way, so the
                // raw cache mirrors the prepared cache for the next comparison.
                next_raw[index] = std::move(lines[index]);
                if (reusable) {
                    lines[index] = previous_prepared[index];
                    continue;
                }
                ++frame_prepare_call_count_;
                auto prepared = detail::prepare_rendered_line(next_raw[index], dimensions.columns);
                if (!prepared) return std::unexpected(prepared.error());
                auto prepared_row = std::move(prepared->text);
                // `prepared->width` is the cell width; `prepared_row.size()` also
                // contains ANSI and OSC bytes and is not a padding bound.
                if (prepared->width < dimensions.columns) {
                    prepared_row.append(dimensions.columns - prepared->width, ' ');
                }
                detail::apply_line_reset(prepared_row);
                lines[index] = std::move(prepared_row);
            }
            return {};
        };
        if (auto result = prepare_frame_rows(new_lines, previous_lines_, previous_raw_lines_, next_raw_lines);
                !result) {
            return std::unexpected(result.error());
        }
        if (has_dock) {
            if (auto result = prepare_frame_rows(
                        new_dock_lines, previous_dock_lines_, previous_raw_dock_lines_, next_raw_dock_lines);
                    !result) {
                return std::unexpected(result.error());
            }
        }
    }

    // pi differential: first-changed-line tracking over the full buffer. The
    // frame's write start is the first changed row or the committed buffer's
    // admitted prefix, whichever is earlier: a partial flush leaves committed
    // rows the terminal has not received yet (#732).
    const auto min_previous = std::min(previous_lines_.size(), new_lines.size());
    std::size_t first_diff = 0;
    while (first_diff < min_previous && previous_lines_[first_diff] == new_lines[first_diff]) {
        ++first_diff;
    }
    std::size_t frame_write_start = std::min(first_diff, admitted_prefix(dimensions, viewport_height));
    // Physical progress of this frame: `admitted_rows` is the first composed
    // row the terminal has not received. A typed Busy refusal records it and
    // marks the frame a resumable partial flush: the rows behind the refusal
    // are already on the terminal, so the retry resumes after them (#732).
    // Every other failure endpoint keeps the all-or-nothing rollback.
    // `stale_previous_rows` keeps the pre-shrink buffer height whose rows below
    // the committed buffer are still owed a clear; `cleared_frame` records that
    // this frame's composition assumes a cleared screen.
    std::size_t admitted_rows = frame_write_start;
    bool partial_flush = false;
    bool cleared_frame = false;
    const auto stale_previous_rows = std::max(previous_lines_.size(), admitted_.stale_below);
    std::size_t stale_cleared_row = admitted_.stale_cleared;

    const auto note_backpressure = [&](const support::Error& error,
                                           std::optional<std::size_t> admitted = std::nullopt) {
        if (error.code != support::ErrorCode::Busy) return;
        admitted_rows = admitted.value_or(new_lines.size());
        partial_flush = true;
    };

    // Write one composed buffer row and record the frame's admitted prefix.
    // The differential drops the row's overlapping image placements first
    // (fork-B image-follows-content); a full-buffer pass leaves placements to
    // the image reconciliation that follows a completed frame, as before.
    const auto write_row_at = [&](std::size_t row, bool remove_images) -> support::ExpectedVoid {
        if (remove_images) {
            const CellRegion region{
                    .column = 0,
                    .row = row,
                    .columns = dimensions.columns,
                    .rows = 1,
            };
            if (auto result = remove_images_intersecting(region); !result) {
                note_backpressure(result.error(), row);
                return std::unexpected(result.error());
            }
        }
        if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
            note_backpressure(result.error(), row);
            return std::unexpected(result.error());
        }
        admitted_rows = row + 1;
        return {};
    };

    // Clear rows [from, to) in place: drop each row's overlapping image
    // placements, then blank the row. A refused row is reported through
    // `on_refusal` before the backpressure note so a resumable clear records
    // the first row it has not cleared (#597, #732).
    const auto clear_rows_in_place = [&](std::size_t from, std::size_t to, auto&& on_refusal) -> support::ExpectedVoid {
        for (std::size_t row = from; row < to; ++row) {
            const CellRegion region{
                    .column = 0,
                    .row = row,
                    .columns = dimensions.columns,
                    .rows = 1,
            };
            if (auto result = remove_images_intersecting(region); !result) {
                on_refusal(row);
                note_backpressure(result.error());
                return std::unexpected(result.error());
            }
            if (auto result = clear_row(terminal_, row, dimensions.columns); !result) {
                on_refusal(row);
                note_backpressure(result.error());
                return std::unexpected(result.error());
            }
        }
        return {};
    };

    const auto supports_sync = capabilities.synchronized_output;
    const auto initial_viewport_top = viewport_top_;
    const auto initial_active_images = active_images_;
    const auto initial_previous_dock_lines = previous_dock_lines_;
    const auto initial_previous_viewport_height = previous_viewport_height_;
    // Viewport bookkeeping rolls back on any failed frame. A partial flush
    // keeps its already-admitted rows and image removals physical (see the
    // failure handling below), so the rollback is split.
    const auto rollback_viewport_state = [&] {
        viewport_top_ = initial_viewport_top;
        previous_dock_lines_ = initial_previous_dock_lines;
        previous_viewport_height_ = initial_previous_viewport_height;
    };
    const auto rollback_render_state = [&] {
        rollback_viewport_state();
        active_images_ = initial_active_images;
    };

    // Begin synchronized update if supported
    if (supports_sync) {
        if (auto result = terminal_.begin_synchronized_update(); !result) {
            return std::unexpected(result.error());
        }
    }

    auto apply_scroll_margins = [&]() -> support::ExpectedVoid {
        // A one-row viewport is a one-row scroll region (0;0): the transcript
        // row stays above the dock instead of the dock overwriting it (#611).
        if (has_dock && viewport_height >= 1) {
            return terminal_.set_scroll_margins(0, viewport_height - 1);
        }
        return terminal_.reset_scroll_margins();
    };

    auto write_dock_lines = [&]() -> support::ExpectedVoid {
        for (std::size_t i = dock_skip; i < new_dock_lines.size(); ++i) {
            if (auto result = write_dock_line(terminal_, i - dock_skip, new_dock_lines[i]); !result) {
                // Reaching the dock means the viewport prefix is complete.
                note_backpressure(result.error());
                return std::unexpected(result.error());
            }
        }
        // Stale-row clearing compares the visible row counts: a previously
        // cropped dock only ever occupied `dock_capacity` rows on screen.
        const std::size_t visible_rows = new_dock_lines.size() - dock_skip;
        const std::size_t previous_visible = std::min(previous_dock_lines_.size(), dock_capacity);
        if (visible_rows < previous_visible) {
            const std::string empty_line(dimensions.columns, ' ');
            for (std::size_t row = visible_rows; row < previous_visible; ++row) {
                if (auto result = write_dock_line(terminal_, row, empty_line); !result) {
                    note_backpressure(result.error());
                    return std::unexpected(result.error());
                }
            }
        }
        return {};
    };

    auto write_full_buffer = [&](std::size_t start_row) -> support::ExpectedVoid {
        if (auto margins = apply_scroll_margins(); !margins) {
            return std::unexpected(margins.error());
        }
        for (std::size_t row = start_row; row < new_lines.size(); ++row) {
            if (auto result = write_row_at(row, /*remove_images=*/false); !result) {
                return std::unexpected(result.error());
            }
        }
        if (has_dock) {
            if (auto result = write_dock_lines(); !result) {
                return std::unexpected(result.error());
            }
        }
        return {};
    };

    // pi fullRender(true): drop all image placements, clear screen, home, and
    // clear scrollback (`\x1b[2J\x1b[H\x1b[3J`), then reflow the full buffer so
    // the terminal's scroll history starts clean. The clear is owed once per
    // composition: a retry after backpressure already cleared and painted its
    // prefix, so it resumes there instead of clearing and re-emitting the
    // buffer from row zero (#732). `force_clear` is the reflow case where rows
    // above the tracked viewport cannot be reached in place (a shrunken buffer
    // or a change above the visible top): the pending prefix does not make that
    // repaint safe, so the screen clears again and the retry resumes from the
    // new attempt's prefix.
    auto clear_and_rewrite = [&](bool force_clear) -> support::ExpectedVoid {
        // The clear homes the buffer at the screen top, physically in this
        // frame or in the attempt that admitted the current prefix. Only a
        // prefix whose remaining rows are all at or below the visible top can
        // resume: rows above it have scrolled into the terminal's scrollback
        // and addressing them in place would clamp them onto the visible top.
        cleared_frame = true;
        const auto previous_visible_top = viewport_top_;
        viewport_top_ = 0;
        const auto resumable = !force_clear && admitted_prefix(dimensions, viewport_height) != 0 && admitted_.cleared &&
                               frame_write_start >= previous_visible_top;
        if (!resumable) {
            if (auto result = remove_active_images(); !result) {
                return std::unexpected(result.error());
            }
            if (auto result = terminal_.clear_screen(); !result) {
                return std::unexpected(result.error());
            }
            // The clear erased every physical row, so the watermark resets
            // only once the clear itself is admitted.
            admitted_ = {};
            frame_write_start = 0;
            admitted_rows = 0;
        }
        return write_full_buffer(frame_write_start);
    };

    // The composed buffer rows visible on screen this frame (the dock owns
    // the rest); shared by the repartition guard above and the viewport-top
    // tracking below so the visible-window rule lives in one place.
    const auto visible_window_rows = has_dock ? viewport_height : dimensions.rows;

    auto render_result = [&]() -> support::ExpectedVoid {
        if (first_render) {
            // pi fullRender(false): write the full buffer without clearing
            // ("assumes clean screen"), so startup content stays visible until
            // the buffer grows past one screen and scrolls away.
            if (auto result = write_full_buffer(frame_write_start); !result) {
                return std::unexpected(result.error());
            }
            if (!has_dock) {
                // Leave the cursor at the end of the written content (column 0) so
                // the terminal advances its scrollback past the rendered content.
                if (auto result = terminal_.set_cursor(
                            CursorPosition{.column = 0, .row = new_lines.empty() ? 0U : new_lines.size() - 1});
                        !result) {
                    note_backpressure(result.error());
                    return std::unexpected(result.error());
                }
            }
            return {};
        }

        // A width or height change reflows from a clean screen — clear screen,
        // home, clear scrollback — so reflow starts clean and the terminal's
        // scroll history is cleared, matching pi (the Termux height-change
        // special-case is not applicable and is not ported).
        if (width_changed || height_changed) {
            return clear_and_rewrite(/*force_clear=*/false);
        }
        // A viewport/dock re-partition repaints the buffer at the new
        // partition without clearing scrollback: every dock row is rewritten
        // at its new address. Rows the shrunken dock vacated inside the
        // grown viewport still show stale dock pixels (they are past the new
        // transcript end, so no buffer line repaints them): clear that gap
        // in place like the differential clear-on-shrink below (#597).
        if (viewport_height_changed) {
            // A grown viewport over a scrolled transcript moves the visible
            // window above the tracked top: those rows live in the terminal's
            // scrollback and address in place only as a clamp onto the visible
            // top, so repaint the partition from a clean screen like any other
            // change above the viewport instead of leaving stale dock pixels.
            // (`admitted_prefix` is 0 on any viewport change, so the rewrite
            // below never resumes a partial prefix; `force_clear` only states
            // that, matching the above-viewport path.)
            const auto new_visible_top = new_lines.size() > visible_window_rows ? new_lines.size() - visible_window_rows
                                                                                 : std::size_t{0};
            if (new_visible_top < viewport_top_) {
                return clear_and_rewrite(/*force_clear=*/true);
            }
            // Rewriting the full buffer overwrites image cells in the terminal,
            // even when the image's logical content and region are unchanged.
            // Retire placements first so image reconciliation at frame end
            // re-places them after the row writes (status/editor dock changes
            // can repartition the viewport without changing transcript content).
            if (auto result = remove_active_images(); !result) {
                return std::unexpected(result.error());
            }
            if (auto result = write_full_buffer(frame_write_start); !result) {
                return std::unexpected(result.error());
            }
            // The full buffer above is already admitted, so a refusal here
            // resumes at the repaint rather than at a tracked stale row.
            if (auto result = clear_rows_in_place(new_lines.size(), viewport_height, [](std::size_t) {}); !result) {
                return std::unexpected(result.error());
            }
            return {};
        }
        if (auto margins = apply_scroll_margins(); !margins) {
            return std::unexpected(margins.error());
        }

        // pi differential: first-changed-line tracking over the full buffer.
        // A row the terminal has not received yet counts as changed: the frame
        // must finish the admitted prefix's tail before it can be skipped. A
        // shrink's stale tail rows count as changed until their clear runs.
        const auto stale_start = std::max(new_lines.size(), stale_cleared_row);
        const auto stale_end =
                std::min(stale_previous_rows, viewport_top_ + (has_dock ? viewport_height : dimensions.rows));
        const auto stale_clear_owed = stale_start < stale_end;
        const auto viewport_unchanged = first_diff == min_previous && previous_lines_.size() == new_lines.size() &&
                                        frame_write_start == new_lines.size() && !stale_clear_owed;
        const auto dock_unchanged = !has_dock || (previous_dock_lines_ == new_dock_lines);
        if (viewport_unchanged && dock_unchanged) return {};

        if (!viewport_unchanged) {

            // A change above the tracked viewport, or new content that ends above
            // it, cannot be reached with line flow: reflow from a clean screen
            // (pi `firstChanged < viewportTop` / `targetRow < viewportTop` full
            // redraw).
            const auto target_row = new_lines.empty() ? 0U : new_lines.size() - 1;
            if (first_diff < viewport_top_ || target_row < viewport_top_) {
                return clear_and_rewrite(/*force_clear=*/true);
            }

            // Line-flow differential: write changed and appended lines from
            // the frame's write start through the end of the buffer. Rows at or
            // past the visible bottom advance the terminal's scrollback (the
            // absolute-cursor seam scrolls on addressing a row below the
            // viewport).
            for (std::size_t row = frame_write_start; row < new_lines.size(); ++row) {
                if (auto result = write_row_at(row, /*remove_images=*/true); !result) {
                    return std::unexpected(result.error());
                }
            }

            // Clear-on-shrink: stale rows below the new content that are still
            // inside the visible viewport are cleared in place (rows that already
            // scrolled into the terminal's scrollback keep their history). A
            // retry resumes the tail at the first row it has not cleared.
            if (auto result = clear_rows_in_place(
                        stale_start, stale_end, [&](std::size_t row) { stale_cleared_row = row; });
                    !result) {
                return std::unexpected(result.error());
            }
            stale_cleared_row = stale_end;
        }

        if (has_dock) {
            if (auto result = write_dock_lines(); !result) {
                return std::unexpected(result.error());
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
    viewport_top_ = std::max(viewport_top_,
        new_lines.size() > visible_window_rows ? new_lines.size() - visible_window_rows : 0U);

    // Position IME cursor based on focused component
    if (render_result) {
        auto cursor_loc = cursor_location;
        if (cursor_loc) {
            if (has_dock) {
                std::size_t dock_row = cursor_loc->row;
                if (dock_row >= viewport_height && dock_row < dimensions.rows) {
                    dock_row -= viewport_height;
                }
                if (dock_row < new_dock_lines.size()) {
                    // The crop shifts every dock row up; a cursor on a
                    // cropped row clamps to the first visible dock row.
                    const std::size_t visible_row = dock_row > dock_skip ? dock_row - dock_skip : 0;
                    if (auto cursor_result = terminal_.set_dock_cursor(visible_row, cursor_loc->column);
                            !cursor_result) {
                        note_backpressure(cursor_result.error());
                        render_result = std::unexpected(cursor_result.error());
                    }
                } else {
                    const auto viewport_bottom = viewport_top_ + viewport_height - 1;
                    if (cursor_loc->row < viewport_top_)
                        cursor_loc->row = viewport_top_;
                    else if (cursor_loc->row > viewport_bottom)
                        cursor_loc->row = viewport_bottom;
                    if (auto cursor_result = terminal_.set_cursor(*cursor_loc); !cursor_result) {
                        note_backpressure(cursor_result.error());
                        render_result = std::unexpected(cursor_result.error());
                    }
                }
            } else {
                const auto viewport_bottom = viewport_top_ + dimensions.rows - 1;
                if (cursor_loc->row < viewport_top_)
                    cursor_loc->row = viewport_top_;
                else if (cursor_loc->row > viewport_bottom)
                    cursor_loc->row = viewport_bottom;
                if (auto cursor_result = terminal_.set_cursor(*cursor_loc); !cursor_result) {
                    note_backpressure(cursor_result.error());
                    render_result = std::unexpected(cursor_result.error());
                }
            }
        }
    }

    if (supports_sync) {
        if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
            rollback_render_state();
            if (!render_result) return std::unexpected(render_result.error());
            return std::unexpected(end_result.error());
        }
    }

    if (!render_result) {
        if (partial_flush) {
            // The terminal admitted the frame's prefix before refusing the next
            // write, so the composed buffer becomes the committed baseline with
            // that prefix marked: the retry resumes after the admitted rows
            // instead of re-emitting the buffer from its first changed row
            // (#732). The rows are physically there and the frame's image
            // removals already happened, so both stay committed. A cleared
            // frame leaves the buffer homed at the screen top, and a shrink
            // leaves its stale rows owed a clear.
            previous_lines_ = std::move(new_lines);
            previous_raw_lines_ = std::move(next_raw_lines);
            rollback_viewport_state();
            if (cleared_frame) {
                // The clear homed the buffer at the screen top; the admitted
                // rows advanced the terminal's scrollback past the visible top.
                const auto visible_rows = has_dock ? viewport_height : dimensions.rows;
                viewport_top_ = admitted_rows > visible_rows ? admitted_rows - visible_rows : 0;
                // The clear also erased the previously drawn dock rows, so the
                // retry must repaint them even when the dock content is
                // unchanged.
                previous_dock_lines_.clear();
            }
            const auto committed_rows = previous_lines_.size();
            admitted_ = {
                    .rows = admitted_rows,
                    .dimensions = dimensions,
                    .viewport_height = viewport_height,
                    .cleared = cleared_frame,
                    .stale_below = !cleared_frame && stale_previous_rows > committed_rows ? stale_previous_rows
                                                                                          : std::size_t{0},
                    .stale_cleared =
                            !cleared_frame && stale_cleared_row > committed_rows ? stale_cleared_row : std::size_t{0},
            };
        } else {
            rollback_render_state();
        }
        return std::unexpected(render_result.error());
    }

    // Update cached state
    previous_lines_ = std::move(new_lines);
    previous_dock_lines_ = std::move(new_dock_lines);
    // The raw caches commit with the prepared caches: a failed render must leave
    // both describing the same frame, or a later matching row would reuse a
    // prepared row the terminal never received.
    previous_raw_lines_ = std::move(next_raw_lines);
    previous_raw_dock_lines_ = std::move(next_raw_dock_lines);
    admitted_ = {
            .rows = previous_lines_.size(),
            .dimensions = dimensions,
            .viewport_height = viewport_height,
    };
    previous_viewport_height_ = viewport_height;
    previous_dimensions_ = dimensions;
    first_render_ = false;
    return {};
}

support::ExpectedVoid RenderPipeline::remove_active_images() {
    while (!active_images_.empty()) {
        const auto image = active_images_.back();
        if (auto removed = terminal_.remove_image(image.handle, image.region); !removed) {
            return std::unexpected(removed.error());
        }
        active_images_.pop_back();
    }
    return {};
}

support::ExpectedVoid RenderPipeline::remove_images_intersecting(const CellRegion& region) {
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

support::ExpectedVoid RenderPipeline::remove_stale_images(
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

support::ExpectedVoid RenderPipeline::place_images(const std::vector<InlineImageRenderRegion>& desired_images) {
    for (const auto& image : desired_images) {
        const auto active = std::find_if(active_images_.begin(), active_images_.end(), [&](const auto& candidate) {
            return candidate.resource_id == image.resource_id && candidate.region == image.region;
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
std::size_t RenderPipeline::admitted_prefix(TerminalDimensions dimensions, std::size_t viewport_height) const {
    if (dimensions.columns != admitted_.dimensions.columns || dimensions.rows != admitted_.dimensions.rows ||
            viewport_height != admitted_.viewport_height) {
        return 0;
    }
    return admitted_.rows;
}

} // namespace cch::tui::detail
