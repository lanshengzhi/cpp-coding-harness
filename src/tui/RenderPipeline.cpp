#include "RenderPipeline.hpp"

#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Tui.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/src/tui-main-screen.ts
// Sidecar fork B relative line flow.
// No DSR (\x1b[6n]) query, no dock partition, no absolute scroll margins.
// First frame flows downward from shell cursor; differential updates use
// computeLineDiff and ANSI relative cursor movements.

namespace cch::tui::detail {

RenderPipeline::RenderPipeline(Terminal& terminal) : terminal_(terminal) {}

void RenderPipeline::start(TerminalDimensions dimensions) noexcept {
    if (!restored_state_pending_start_) {
        reset_render_state();
        previous_dimensions_ = dimensions;
        return;
    }

    first_render_ = previous_lines_.empty();
    if (!previous_dimensions_) previous_dimensions_ = dimensions;
    admitted_ = {};
    restored_state_pending_start_ = false;
}

std::size_t RenderPipeline::admitted_prefix(TerminalDimensions dimensions) const noexcept {
    if (dimensions.columns != admitted_.dimensions.columns || dimensions.rows != admitted_.dimensions.rows) {
        return 0;
    }
    return admitted_.rows;
}

void RenderPipeline::begin_frame() noexcept { frame_prepare_call_count_ = 0; }

void RenderPipeline::note_prepared_line() noexcept { ++frame_prepare_call_count_; }

std::size_t RenderPipeline::frame_prepare_call_count() const noexcept { return frame_prepare_call_count_; }

support::ExpectedVoid RenderPipeline::stop(std::optional<CursorPosition>, TuiStopOptions options) {
    support::ExpectedVoid image_result;
    support::ExpectedVoid exit_result;
    if (!options.preserve_screen) {
        const auto delete_seq = delete_kitty_images(previous_kitty_image_ids_);
        if (!delete_seq.empty()) {
            if (auto deleted = terminal_.write(delete_seq); !deleted) {
                image_result = std::unexpected(deleted.error());
            }
        }
        if (!previous_lines_.empty()) {
            std::string buffer{" "};
            const auto target_row = previous_lines_.size();
            const auto line_diff =
                    static_cast<std::ptrdiff_t>(target_row) - static_cast<std::ptrdiff_t>(hardware_cursor_row_);
            if (line_diff > 0) {
                buffer += std::format("\x1b[{}B", line_diff);
            } else if (line_diff < 0) {
                buffer += std::format("\x1b[{}A", -line_diff);
            }
            buffer += "\r\n";
            exit_result = terminal_.write(buffer);
        }
    }

    reset_render_state();
    if (!image_result) return std::unexpected(image_result.error());
    return exit_result;
}

TuiMainScreenRenderState RenderPipeline::capture_render_state() const {
    TuiMainScreenRenderState state;
    state.previous_lines = previous_lines_;
    if (previous_dimensions_) {
        state.previous_width = previous_dimensions_->columns;
        state.previous_height = previous_dimensions_->rows;
    }
    state.cursor_row = cursor_row_;
    state.hardware_cursor_row = hardware_cursor_row_;
    state.max_lines_rendered = max_lines_rendered_;
    state.previous_viewport_top = previous_viewport_top_;
    return state;
}

support::ExpectedVoid RenderPipeline::restore_render_state(TuiMainScreenRenderState state) {
    const bool lines_empty = state.previous_lines.empty();
    const bool width_uninitialized = state.previous_width == 0;
    const bool height_uninitialized = state.previous_height == 0;
    const bool valid_dimensions = width_uninitialized == height_uninitialized;
    const bool valid_content =
            lines_empty
                    ? (state.cursor_row == 0 && state.hardware_cursor_row == 0 && state.max_lines_rendered == 0 &&
                              state.previous_viewport_top == 0)
                    : (!width_uninitialized && state.previous_width > 0 && state.previous_height > 0 &&
                              state.cursor_row < state.previous_lines.size() &&
                              state.hardware_cursor_row <= state.previous_viewport_top + state.previous_height - 1 &&
                              state.max_lines_rendered >= state.previous_lines.size() &&
                              state.previous_viewport_top <= state.previous_lines.size());
    if (!valid_dimensions || !valid_content) {
        reset_render_state();
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Main-screen render state is inconsistent"));
    }

    for (auto& line : state.previous_lines) {
        if (is_image_line(line)) line.clear();
    }
    previous_lines_ = std::move(state.previous_lines);
    previous_raw_lines_.clear();
    if (width_uninitialized) {
        previous_dimensions_.reset();
    } else {
        previous_dimensions_ = TerminalDimensions{
                .columns = state.previous_width,
                .rows = state.previous_height,
        };
    }
    cursor_row_ = state.cursor_row;
    hardware_cursor_row_ = state.hardware_cursor_row;
    max_lines_rendered_ = state.max_lines_rendered;
    previous_viewport_top_ = state.previous_viewport_top;
    previous_kitty_image_ids_.clear();
    image_placements_.clear();
    first_render_ = lines_empty;
    admitted_ = {};
    restored_state_pending_start_ = true;
    return {};
}

void RenderPipeline::reset_render_state() noexcept {
    first_render_ = true;
    previous_lines_.clear();
    previous_raw_lines_.clear();
    cursor_row_ = 0;
    hardware_cursor_row_ = 0;
    max_lines_rendered_ = 0;
    previous_viewport_top_ = 0;
    previous_dimensions_.reset();
    admitted_ = {};
    previous_kitty_image_ids_.clear();
    image_placements_.clear();
    restored_state_pending_start_ = false;
}

support::ExpectedVoid RenderPipeline::clear_screen() {
    const auto delete_seq = delete_kitty_images(previous_kitty_image_ids_);
    if (!delete_seq.empty()) {
        (void)terminal_.write(delete_seq);
    }
    if (auto cleared = terminal_.clear_screen(); !cleared) {
        return std::unexpected(cleared.error());
    }
    reset_render_state();
    return {};
}

void RenderPipeline::position_hardware_cursor(
        const std::optional<CursorPosition>& cursor_pos, std::size_t total_lines) {
    if (!cursor_pos || total_lines == 0) {
        (void)terminal_.set_cursor_visible(false);
        return;
    }

    const std::size_t target_row = std::min(cursor_pos->row, total_lines - 1);
    const std::size_t target_col = cursor_pos->column;

    const auto row_delta = static_cast<std::ptrdiff_t>(target_row) - static_cast<std::ptrdiff_t>(hardware_cursor_row_);
    std::string buffer;
    if (row_delta > 0) {
        buffer += std::format("\x1b[{}B", row_delta);
    } else if (row_delta < 0) {
        buffer += std::format("\x1b[{}A", -row_delta);
    }
    buffer += std::format("\x1b[{}G", target_col + 1);

    if (!buffer.empty()) {
        if (auto result = write_frame(buffer); result) {
            hardware_cursor_row_ = target_row;
        }
    } else {
        hardware_cursor_row_ = target_row;
    }
    cursor_row_ = total_lines == 0 ? 0 : total_lines - 1;
}

support::ExpectedVoid RenderPipeline::write_frame(std::string_view output) {
    ++write_accounting_.attempted_writes;
    auto result = terminal_.write(output);
    if (result) {
        ++write_accounting_.committed_writes;
        write_accounting_.committed_bytes += output.size();
    }
    return result;
}

support::ExpectedVoid RenderPipeline::render(RenderResult materialized,
        TerminalDimensions dimensions,
        TerminalCapabilities capabilities,
        bool compose_overlays,
        bool has_overlay_entries,
        bool clear_on_shrink,
        std::optional<CursorPosition> cursor_location) {
    // Accounting windows one frame: pi builds one bounded writer per render
    // (`BoundedTerminalWriter`, tui-main-screen.ts:18) whose committed
    // character count starts at zero, so a retry reports its own attempts.
    write_accounting_ = {};
    const auto initial_image_placements = image_placements_;
    compose_images(materialized.lines, materialized.images, capabilities.inline_images);
    if (capabilities.inline_images == InlineImageProtocol::None) {
        for (const auto& image : materialized.images) {
            if (image.region.row < materialized.lines.size()) {
                materialized.lines[image.region.row] = image.fallback_text;
            }
        }
    }
    auto& new_lines = materialized.lines;

    const auto initial_first_render = first_render_;
    const auto initial_lines = previous_lines_;
    const auto initial_raw_lines = previous_raw_lines_;
    const auto initial_dimensions = previous_dimensions_;
    const auto initial_max_lines = max_lines_rendered_;
    const auto initial_viewport_top = previous_viewport_top_;
    const auto initial_cursor_row = cursor_row_;
    const auto initial_hardware_cursor_row = hardware_cursor_row_;
    const auto initial_kitty_ids = previous_kitty_image_ids_;

    const auto rollback = [&] {
        first_render_ = initial_first_render;
        previous_lines_ = initial_lines;
        previous_raw_lines_ = initial_raw_lines;
        previous_dimensions_ = initial_dimensions;
        max_lines_rendered_ = initial_max_lines;
        previous_viewport_top_ = initial_viewport_top;
        cursor_row_ = initial_cursor_row;
        hardware_cursor_row_ = initial_hardware_cursor_row;
        previous_kitty_image_ids_ = initial_kitty_ids;
        image_placements_ = initial_image_placements;
    };

    const bool supports_sync = capabilities.synchronized_output;

    const auto height = dimensions.rows;
    const auto width = dimensions.columns;
    const bool width_changed = previous_dimensions_.has_value() && previous_dimensions_->columns != width;
    const bool height_changed = previous_dimensions_.has_value() && previous_dimensions_->rows != height;
    const auto first_render = first_render_;

    const std::size_t previous_buffer_length =
            previous_dimensions_.has_value() ? previous_viewport_top_ + previous_dimensions_->rows : height;
    std::size_t prev_viewport_top = height_changed
                                            ? (previous_buffer_length >= height ? previous_buffer_length - height : 0)
                                            : previous_viewport_top_;
    std::size_t viewport_top = prev_viewport_top;
    std::size_t hardware_cursor_row = hardware_cursor_row_;

    const auto compute_line_diff = [&](std::size_t target_row) -> std::ptrdiff_t {
        const auto current_screen_row =
                static_cast<std::ptrdiff_t>(hardware_cursor_row) - static_cast<std::ptrdiff_t>(prev_viewport_top);
        const auto target_screen_row =
                static_cast<std::ptrdiff_t>(target_row) - static_cast<std::ptrdiff_t>(viewport_top);
        return target_screen_row - current_screen_row;
    };

    const auto pad_and_reset_rows = [&](std::vector<std::string>& lines) {
        std::size_t skip_reserved = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto& line = lines[i];
            if (is_image_line(line)) {
                skip_reserved = get_kitty_image_reserved_rows(lines, i, lines.empty() ? 0 : lines.size() - 1);
                if (skip_reserved > 0) --skip_reserved;
                continue;
            }
            if (skip_reserved > 0) {
                --skip_reserved;
                continue;
            }
            const auto visible_columns = detail::measure_visible_width(line).width;
            if (visible_columns < dimensions.columns) {
                line.append(dimensions.columns - visible_columns, ' ');
            }
            detail::apply_line_reset(line);
        }
    };

    std::vector<std::string> next_raw_lines;
    if (compose_overlays) {
        pad_and_reset_rows(new_lines);
    } else {
        const bool rows_reusable = !first_render && !width_changed;
        next_raw_lines.resize(new_lines.size());
        std::size_t skip_reserved = 0;
        for (std::size_t index = 0; index < new_lines.size(); ++index) {
            const bool is_image = is_image_line(new_lines[index]);
            if (is_image) {
                skip_reserved =
                        get_kitty_image_reserved_rows(new_lines, index, new_lines.empty() ? 0 : new_lines.size() - 1);
                if (skip_reserved > 0) --skip_reserved;
                next_raw_lines[index] = new_lines[index];
                continue;
            }
            if (skip_reserved > 0) {
                --skip_reserved;
                next_raw_lines[index] = new_lines[index];
                continue;
            }
            const bool reusable = rows_reusable && index < previous_lines_.size() &&
                                  index < previous_raw_lines_.size() && new_lines[index] == previous_raw_lines_[index];
            next_raw_lines[index] = std::move(new_lines[index]);
            if (reusable) {
                new_lines[index] = previous_lines_[index];
                continue;
            }
            ++frame_prepare_call_count_;
            auto prepared = detail::prepare_rendered_line(next_raw_lines[index], dimensions.columns);
            if (!prepared) {
                rollback();
                return std::unexpected(prepared.error());
            }
            auto prepared_row = std::move(prepared->text);
            if (prepared->width < dimensions.columns) {
                prepared_row.append(dimensions.columns - prepared->width, ' ');
            }
            detail::apply_line_reset(prepared_row);
            new_lines[index] = std::move(prepared_row);
        }
    }

    const auto min_previous = std::min(previous_lines_.size(), new_lines.size());
    std::size_t first_diff = 0;
    while (first_diff < min_previous && previous_lines_[first_diff] == new_lines[first_diff]) {
        ++first_diff;
    }
    const std::size_t admitted_rows = admitted_prefix(dimensions);
    std::size_t frame_write_start = std::min(first_diff, admitted_rows);
    std::size_t admitted_watermark = frame_write_start;
    bool partial_flush = false;
    bool cleared_frame = false;

    const auto note_backpressure = [&](const support::Error& error,
                                           std::optional<std::size_t> admitted = std::nullopt) {
        if (error.code != support::ErrorCode::Busy) return;
        admitted_watermark = admitted.value_or(new_lines.size());
        partial_flush = true;
    };

    const auto finish = [&](support::ExpectedVoid result) -> support::ExpectedVoid {
        if (!result) {
            if (partial_flush) {
                if (admitted_watermark < new_lines.size()) {
                    new_lines.resize(admitted_watermark);
                }
                if (admitted_watermark < next_raw_lines.size()) {
                    next_raw_lines.resize(admitted_watermark);
                }
                previous_lines_ = std::move(new_lines);
                previous_raw_lines_ = std::move(next_raw_lines);
                cursor_row_ = admitted_watermark > 0 ? admitted_watermark - 1 : 0;
                hardware_cursor_row_ = admitted_watermark > 0 ? admitted_watermark - 1 : 0;
                max_lines_rendered_ = std::max(max_lines_rendered_, admitted_watermark);
                const auto buffer_length = std::max(height, admitted_watermark);
                previous_viewport_top_ = buffer_length > height ? buffer_length - height : 0;
                previous_dimensions_ = dimensions;
                first_render_ = false;
                admitted_ = {
                        .rows = admitted_watermark,
                        .dimensions = dimensions,
                        .cleared = cleared_frame,
                };
            } else {
                rollback();
            }
            return result;
        }
        admitted_ = {
                .rows = previous_lines_.size(),
                .dimensions = dimensions,
                .cleared = false,
        };
        return result;
    };

    auto full_render = [&](bool clear) -> support::ExpectedVoid {
        // Frozen `fullRender` counts the attempt before it writes anything
        // (tui-main-screen.ts:277), so a refused frame still counts.
        ++full_redraws_;
        // Frozen `fullRender` opens the synchronized-output block before it
        // clears: the delete/clear bytes are part of the same atomic update.
        const bool resumable = clear && admitted_.cleared && admitted_prefix(dimensions) != 0;
        if (supports_sync) {
            if (auto begin_result = terminal_.begin_synchronized_update(); !begin_result) {
                rollback();
                return std::unexpected(begin_result.error());
            }
        }
        if (clear && !resumable) {
            const auto delete_seq = delete_kitty_images(previous_kitty_image_ids_);
            if (!delete_seq.empty()) {
                if (auto del_res = write_frame(delete_seq); !del_res) {
                    if (supports_sync) (void)terminal_.end_synchronized_update();
                    rollback();
                    return std::unexpected(del_res.error());
                }
            }
            if (auto cleared = terminal_.clear_screen(); !cleared) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                rollback();
                return std::unexpected(cleared.error());
            }
            cleared_frame = true;
            admitted_ = {};
            frame_write_start = 0;
            admitted_watermark = 0;
            hardware_cursor_row_ = 0;
            previous_viewport_top_ = 0;
            hardware_cursor_row = 0;
            prev_viewport_top = 0;
            viewport_top = 0;
        } else if (resumable) {
            cleared_frame = true;
            viewport_top = 0;
            prev_viewport_top = 0;
            hardware_cursor_row = hardware_cursor_row_;
        }
        for (std::size_t i = frame_write_start; i < new_lines.size(); ++i) {
            std::string line_to_write;
            if (i > 0) line_to_write += "\r\n";
            const auto& line = new_lines[i];
            const bool is_image = is_image_line(line);
            const auto image_reserved_rows =
                    is_image ? get_kitty_image_reserved_rows(new_lines, i, new_lines.empty() ? 0 : new_lines.size() - 1)
                             : 1;
            if (image_reserved_rows > 1 && image_reserved_rows <= height) {
                for (std::size_t row = 1; row < image_reserved_rows; ++row) {
                    line_to_write += "\r\n";
                }
                line_to_write += std::format("\x1b[{}A", image_reserved_rows - 1);
                line_to_write += line;
                line_to_write += std::format("\x1b[{}B", image_reserved_rows - 1);
                if (auto result = write_frame(line_to_write); !result) {
                    // The reserved-row group is one write: a refusal leaves the
                    // whole group unadmitted, so the retry rewrites it.
                    note_backpressure(result.error(), i);
                    if (supports_sync) (void)terminal_.end_synchronized_update();
                    return std::unexpected(result.error());
                }
                admitted_watermark = i + image_reserved_rows;
                hardware_cursor_row = i + image_reserved_rows - 1;
                i += image_reserved_rows - 1;
                continue;
            }
            line_to_write += line;
            if (auto result = write_frame(line_to_write); !result) {
                note_backpressure(result.error(), i);
                if (supports_sync) (void)terminal_.end_synchronized_update();
                return std::unexpected(result.error());
            }
            admitted_watermark = i + 1;
            hardware_cursor_row = i;
        }
        if (supports_sync) {
            if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
                rollback();
                return std::unexpected(end_result.error());
            }
        }
        cursor_row_ = new_lines.empty() ? 0 : new_lines.size() - 1;
        hardware_cursor_row_ = new_lines.empty() ? 0 : new_lines.size() - 1;
        if (clear) {
            max_lines_rendered_ = new_lines.size();
        } else {
            max_lines_rendered_ = std::max(max_lines_rendered_, new_lines.size());
        }
        const auto buffer_length = std::max(height, new_lines.size());
        previous_viewport_top_ = buffer_length > height ? buffer_length - height : 0;

        position_hardware_cursor(cursor_location, new_lines.size());
        previous_kitty_image_ids_ = collect_kitty_image_ids(new_lines);
        previous_lines_ = std::move(new_lines);
        previous_raw_lines_ = std::move(next_raw_lines);
        previous_dimensions_ = dimensions;
        first_render_ = false;
        admitted_ = {
                .rows = previous_lines_.size(),
                .dimensions = dimensions,
                .cleared = false,
        };
        return {};
    };

    // 1. First render
    if ((first_render || previous_lines_.empty()) && !width_changed && !height_changed) {
        return finish(full_render(false));
    }

    // 2. Width changed
    if (width_changed) {
        return finish(full_render(true));
    }

    // 3. Height changed
    if (height_changed) {
        return finish(full_render(true));
    }

    // 4. clearOnShrink
    if (clear_on_shrink && new_lines.size() < max_lines_rendered_ && !has_overlay_entries) {
        return finish(full_render(true));
    }

    // 5. Compute diff
    std::ptrdiff_t first_changed = -1;
    std::ptrdiff_t last_changed = -1;
    const auto max_lines = std::max(new_lines.size(), previous_lines_.size());
    for (std::size_t i = 0; i < max_lines; ++i) {
        const std::string_view old_line = i < previous_lines_.size() ? previous_lines_[i] : std::string_view{};
        const std::string_view new_line = i < new_lines.size() ? new_lines[i] : std::string_view{};
        if (old_line != new_line) {
            if (first_changed == -1) first_changed = static_cast<std::ptrdiff_t>(i);
            last_changed = static_cast<std::ptrdiff_t>(i);
        }
    }
    for (const auto& img : materialized.images) {
        auto it = initial_image_placements.find(img.resource_id);
        if (it != initial_image_placements.end() && it->second.revision != img.revision) {
            const auto row = static_cast<std::ptrdiff_t>(img.region.row);
            if (first_changed == -1 || row < first_changed) first_changed = row;
            if (row > last_changed) last_changed = row;
        }
    }
    const bool appended_lines = new_lines.size() > previous_lines_.size();
    if (appended_lines) {
        if (first_changed == -1) first_changed = static_cast<std::ptrdiff_t>(previous_lines_.size());
        last_changed = static_cast<std::ptrdiff_t>(new_lines.size() - 1);
    }
    const bool append_start =
            appended_lines && first_changed == static_cast<std::ptrdiff_t>(previous_lines_.size()) && first_changed > 0;

    if (first_changed != -1) {
        const auto [exp_first, exp_last] =
                expand_changed_range_for_kitty_images(first_changed, last_changed, previous_lines_, new_lines);
        first_changed = exp_first;
        last_changed = exp_last;
    }

    // 6. No changes - only reposition hardware cursor (no synchronized output)
    if (first_changed == -1) {
        position_hardware_cursor(cursor_location, new_lines.size());
        previous_viewport_top_ = prev_viewport_top;
        previous_dimensions_ = dimensions;
        admitted_ = {
                .rows = previous_lines_.size(),
                .dimensions = dimensions,
                .cleared = false,
        };
        return finish(support::ExpectedVoid{});
    }

    // 7. All changes are in deleted lines
    if (first_changed >= static_cast<std::ptrdiff_t>(new_lines.size())) {
        if (previous_lines_.size() > new_lines.size()) {
            const std::size_t target_row = new_lines.empty() ? 0 : new_lines.size() - 1;
            if (target_row < prev_viewport_top) {
                return finish(full_render(true));
            }
            const auto line_diff = compute_line_diff(target_row);
            const auto extra_lines = previous_lines_.size() - new_lines.size();
            if (extra_lines > height) {
                return finish(full_render(true));
            }
            std::string buffer;
            buffer += delete_changed_kitty_images(first_changed, last_changed, previous_lines_);
            if (line_diff > 0) {
                buffer += std::format("\x1b[{}B", line_diff);
            } else if (line_diff < 0) {
                buffer += std::format("\x1b[{}A", -line_diff);
            }
            buffer += "\r";
            const std::size_t clear_start_offset = new_lines.empty() ? 0 : 1;
            if (extra_lines > 0 && clear_start_offset > 0) {
                buffer += std::format("\x1b[{}B", clear_start_offset);
            }
            for (std::size_t i = 0; i < extra_lines; ++i) {
                buffer += "\r\x1b[2K";
                if (i < extra_lines - 1) buffer += "\x1b[1B";
            }
            const std::size_t move_back = extra_lines > 0 ? (extra_lines - 1 + clear_start_offset) : 0;
            if (move_back > 0) {
                buffer += std::format("\x1b[{}A", move_back);
            }
            if (supports_sync) {
                if (auto begin_result = terminal_.begin_synchronized_update(); !begin_result) {
                    rollback();
                    return finish(std::unexpected(begin_result.error()));
                }
            }
            if (auto result = write_frame(buffer); !result) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                rollback();
                return finish(std::unexpected(result.error()));
            }
            if (supports_sync) {
                if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
                    rollback();
                    return finish(std::unexpected(end_result.error()));
                }
            }
            cursor_row_ = target_row;
            hardware_cursor_row_ = target_row;
        }
        cursor_row_ = new_lines.empty() ? 0 : new_lines.size() - 1;
        position_hardware_cursor(cursor_location, new_lines.size());
        previous_kitty_image_ids_ = collect_kitty_image_ids(new_lines);
        previous_lines_ = std::move(new_lines);
        previous_raw_lines_ = std::move(next_raw_lines);
        previous_dimensions_ = dimensions;
        previous_viewport_top_ = prev_viewport_top;
        admitted_ = {
                .rows = previous_lines_.size(),
                .dimensions = dimensions,
                .cleared = false,
        };
        return finish(support::ExpectedVoid{});
    }

    // 8. firstChanged < prevViewportTop
    if (first_changed < static_cast<std::ptrdiff_t>(prev_viewport_top)) {
        return finish(full_render(true));
    }

    // 9. Differential write
    if (supports_sync) {
        if (auto begin_result = terminal_.begin_synchronized_update(); !begin_result) {
            rollback();
            return finish(std::unexpected(begin_result.error()));
        }
    }

    const std::size_t prev_viewport_bottom = prev_viewport_top + height - 1;
    const std::size_t move_target_row =
            append_start ? static_cast<std::size_t>(first_changed - 1) : static_cast<std::size_t>(first_changed);

    std::string buffer;
    buffer += delete_changed_kitty_images(first_changed, last_changed, previous_lines_);

    if (move_target_row > prev_viewport_bottom) {
        const std::size_t current_screen_row = std::min(height - 1,
                hardware_cursor_row >= prev_viewport_top ? hardware_cursor_row - prev_viewport_top : std::size_t{0});
        const std::size_t move_to_bottom = height - 1 - current_screen_row;
        if (move_to_bottom > 0) {
            buffer += std::format("\x1b[{}B", move_to_bottom);
        }
        const std::size_t scroll = move_target_row - prev_viewport_bottom;
        for (std::size_t s = 0; s < scroll; ++s) {
            buffer += "\r\n";
        }
        prev_viewport_top += scroll;
        viewport_top += scroll;
        hardware_cursor_row = move_target_row;
    }

    const auto line_diff = compute_line_diff(move_target_row);
    if (line_diff > 0) {
        buffer += std::format("\x1b[{}B", line_diff);
    } else if (line_diff < 0) {
        buffer += std::format("\x1b[{}A", -line_diff);
    }

    buffer += append_start ? "\r\n" : "\r";

    const std::size_t render_end = std::min(static_cast<std::size_t>(last_changed), new_lines.size() - 1);
    for (std::size_t i = static_cast<std::size_t>(first_changed); i <= render_end; ++i) {
        if (i > static_cast<std::size_t>(first_changed)) buffer += "\r\n";
        const auto& line = new_lines[i];
        const bool is_image = is_image_line(line);
        const auto image_reserved_rows = is_image ? get_kitty_image_reserved_rows(new_lines, i, render_end) : 1;
        if (image_reserved_rows > 1) {
            const auto image_start_screen_row =
                    static_cast<std::ptrdiff_t>(i) - static_cast<std::ptrdiff_t>(viewport_top);
            if (image_start_screen_row < 0 ||
                    static_cast<std::size_t>(image_start_screen_row) + image_reserved_rows > height) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                return full_render(true);
            }
            buffer += "\x1b[2K";
            for (std::size_t row = 1; row < image_reserved_rows; ++row) {
                buffer += "\r\n\x1b[2K";
            }
            buffer += std::format("\x1b[{}A", image_reserved_rows - 1);
            buffer += line;
            buffer += std::format("\x1b[{}B", image_reserved_rows - 1);
            i += image_reserved_rows - 1;
            continue;
        }
        buffer += "\x1b[2K";
        buffer += line;
    }

    std::size_t final_cursor_row = render_end;
    if (previous_lines_.size() > new_lines.size()) {
        if (render_end < new_lines.size() - 1) {
            const auto move_down = new_lines.size() - 1 - render_end;
            buffer += std::format("\x1b[{}B", move_down);
            final_cursor_row = new_lines.size() - 1;
        }
        const auto extra_lines = previous_lines_.size() - new_lines.size();
        for (std::size_t i = new_lines.size(); i < previous_lines_.size(); ++i) {
            buffer += "\r\n\x1b[2K";
        }
        buffer += std::format("\x1b[{}A", extra_lines);
    }

    if (auto result = write_frame(buffer); !result) {
        if (supports_sync) (void)terminal_.end_synchronized_update();
        rollback();
        return finish(std::unexpected(result.error()));
    }

    if (supports_sync) {
        if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
            rollback();
            return finish(std::unexpected(end_result.error()));
        }
    }

    cursor_row_ = new_lines.empty() ? 0 : new_lines.size() - 1;
    hardware_cursor_row_ = final_cursor_row;
    max_lines_rendered_ = std::max(max_lines_rendered_, new_lines.size());
    previous_viewport_top_ =
            std::max(prev_viewport_top, final_cursor_row >= height ? final_cursor_row - height + 1 : std::size_t{0});

    position_hardware_cursor(cursor_location, new_lines.size());
    previous_kitty_image_ids_ = collect_kitty_image_ids(new_lines);
    previous_lines_ = std::move(new_lines);
    previous_raw_lines_ = std::move(next_raw_lines);
    previous_dimensions_ = dimensions;
    admitted_ = {
            .rows = previous_lines_.size(),
            .dimensions = dimensions,
            .cleared = false,
    };
    return finish(support::ExpectedVoid{});
}

void RenderPipeline::clear_image_placements() noexcept {
    fullscreen_image_placements_.clear();
    fullscreen_image_order_.clear();
}

FullscreenImageFrame RenderPipeline::prepare_fullscreen_images(std::vector<std::string> lines,
        const std::vector<InlineImageRenderRegion>& images,
        InlineImageProtocol protocol,
        bool full_redraw) {
    FullscreenImageFrame frame{
            .lines = std::move(lines), .clear_bytes = {}, .deletion_bytes = {}, .images_need_redraw = false};
    if (protocol != InlineImageProtocol::Kitty) {
        // Frozen fullscreen sessions on iTerm2 null out the image capability
        // for the whole session: the component's text fallback shows and no
        // image sequence is emitted (pi `beforeTerminalStart`).
        for (const auto& image : images) {
            if (image.region.row < frame.lines.size()) {
                frame.lines[image.region.row] = image.fallback_text;
            }
        }
        return frame;
    }

    // Frozen picks the full-redraw clear from the cache as it stood before this
    // frame's uploads are cached (pi `hadUploadedKittyImages`).
    const bool had_uploaded_images = !fullscreen_image_placements_.empty();

    for (auto& [resource_id, placement] : fullscreen_image_placements_)
        placement.visible = false;
    bool need_redraw = false;
    // Placement rows for the LRU touch order: frozen walks the finished screen
    // top-to-bottom, not the paint order (pi `prepareKittyScreen`).
    std::vector<std::pair<std::size_t, std::uint64_t>> visible_resources;
    visible_resources.reserve(images.size());
    for (const auto& image : images) {
        if (image.region.columns == 0 || image.region.rows == 0) continue;
        if (image.region.row >= frame.lines.size()) continue;
        if (frame.lines.size() < image.region.row + image.region.rows) {
            frame.lines.resize(image.region.row + image.region.rows);
        }

        std::optional<std::string_view> filename;
        if (image.filename) filename = *image.filename;
        TerminalImage term_img{
                .encoded_data = image.encoded_data,
                .mime_type = image.mime_type,
                .filename = filename,
                .pixel_width = image.pixel_width,
                .pixel_height = image.pixel_height,
                .resource_id = image.resource_id,
                .revision = image.revision,
                .region = image.region,
                .source_rows = image.source_rows,
                .source_row_offset = image.source_row_offset,
        };

        if (image.resource_id == 0) {
            // No session identity to reuse: frozen transmits and places again,
            // and the changed anchor forces the full rewrite.
            const auto handle = allocate_image_id();
            auto encoded = detail::encode_terminal_image(protocol, term_img, handle);
            if (!encoded) continue;
            frame.lines[image.region.row] = std::string(image.region.column, ' ') + *encoded;
            for (std::size_t row = image.region.row + 1; row < image.region.row + image.region.rows; ++row) {
                frame.lines[row].clear();
            }
            need_redraw = true;
            continue;
        }

        const auto cached = fullscreen_image_placements_.find(image.resource_id);
        std::string anchor;
        if (cached != fullscreen_image_placements_.end() && cached->second.revision == image.revision) {
            anchor = detail::encode_kitty_image_placement(term_img, cached->second.handle);
        } else {
            const auto handle =
                    cached != fullscreen_image_placements_.end() ? cached->second.handle : allocate_image_id();
            auto encoded = detail::encode_terminal_image(protocol, term_img, handle);
            if (!encoded) continue;
            auto& placement = fullscreen_image_placements_[image.resource_id];
            placement.handle = handle;
            placement.revision = image.revision;
            placement.transmission_bytes = encoded->size();
            placement.decoded_bytes = image.pixel_width * image.pixel_height * 4;
            anchor = std::move(*encoded);
        }

        auto& placement = fullscreen_image_placements_[image.resource_id];
        const bool moved = placement.row != image.region.row || placement.column != image.region.column;
        if (!placement.anchored || moved || placement.anchor != anchor) need_redraw = true;
        placement.anchor = anchor;
        placement.row = image.region.row;
        placement.column = image.region.column;
        placement.visible = true;
        visible_resources.emplace_back(image.region.row, image.resource_id);

        frame.lines[image.region.row] = std::string(image.region.column, ' ') + anchor;
        for (std::size_t row = image.region.row + 1; row < image.region.row + image.region.rows; ++row) {
            frame.lines[row].clear();
        }
    }

    for (const auto& [resource_id, placement] : fullscreen_image_placements_) {
        if (placement.anchored && !placement.visible) need_redraw = true;
    }
    for (auto& [resource_id, placement] : fullscreen_image_placements_)
        placement.anchored = placement.visible;

    if (full_redraw) {
        frame.clear_bytes = had_uploaded_images ? delete_all_kitty_placements() : delete_all_kitty_images();
    } else if (need_redraw) {
        frame.clear_bytes = delete_all_kitty_placements();
    }
    frame.images_need_redraw = need_redraw;
    if (!full_redraw && !need_redraw) {
        // Frozen never calls prepareKittyScreen when no image row changed, so
        // the cache keeps its order and nothing is evicted.
        return frame;
    }

    std::stable_sort(visible_resources.begin(), visible_resources.end());
    for (const auto& [resource_row, resource_id] : visible_resources) {
        std::erase(fullscreen_image_order_, resource_id);
        fullscreen_image_order_.push_back(resource_id);
    }

    std::size_t offscreen_count = 0;
    std::size_t transmission_bytes = 0;
    std::size_t decoded_bytes = 0;
    for (const auto& [resource_id, placement] : fullscreen_image_placements_) {
        if (placement.visible) continue;
        ++offscreen_count;
        transmission_bytes += placement.transmission_bytes;
        decoded_bytes += placement.decoded_bytes;
    }
    for (auto it = fullscreen_image_order_.begin(); it != fullscreen_image_order_.end();) {
        if (offscreen_count <= kMaxCachedOffscreenKittyImages &&
                transmission_bytes <= kMaxCachedOffscreenKittyTransmissionBytes &&
                decoded_bytes <= kMaxCachedOffscreenKittyDecodedBytes) {
            break;
        }
        const auto placement = fullscreen_image_placements_.find(*it);
        if (placement == fullscreen_image_placements_.end()) {
            it = fullscreen_image_order_.erase(it);
            continue;
        }
        if (placement->second.visible) {
            ++it;
            continue;
        }
        frame.deletion_bytes += delete_kitty_image(placement->second.handle);
        --offscreen_count;
        transmission_bytes -= placement->second.transmission_bytes;
        decoded_bytes -= placement->second.decoded_bytes;
        fullscreen_image_placements_.erase(placement);
        it = fullscreen_image_order_.erase(it);
    }
    return frame;
}

void RenderPipeline::compose_images(std::vector<std::string>& lines,
        const std::vector<InlineImageRenderRegion>& images,
        InlineImageProtocol protocol) {
    if (protocol != InlineImageProtocol::None) {
        for (const auto& image : images) {
            const auto start_row = image.region.row;
            const auto num_rows = image.region.rows;
            const auto start_col = image.region.column;
            if (num_rows == 0) continue;
            if (lines.size() < start_row + num_rows) {
                lines.resize(start_row + num_rows);
            }

            std::optional<std::string_view> filename;
            if (image.filename) filename = *image.filename;

            TerminalImage term_img{
                    .encoded_data = image.encoded_data,
                    .mime_type = image.mime_type,
                    .filename = filename,
                    .pixel_width = image.pixel_width,
                    .pixel_height = image.pixel_height,
                    .resource_id = image.resource_id,
                    .revision = image.revision,
                    .region = image.region,
            };

            TerminalImageHandle handle{0};
            if (image.resource_id > 0) {
                auto it = image_placements_.find(image.resource_id);
                if (it != image_placements_.end() && it->second.region == image.region) {
                    handle = it->second.handle;
                    it->second.revision = image.revision;
                } else {
                    handle = allocate_image_id();
                    image_placements_[image.resource_id] = {
                            .handle = handle,
                            .region = image.region,
                            .revision = image.revision,
                    };
                }
            } else {
                handle = allocate_image_id();
            }

            if (protocol == InlineImageProtocol::Kitty) {
                auto encoded = detail::encode_terminal_image(protocol, term_img, handle);
                if (!encoded) continue;
                lines[start_row] = std::string(start_col, ' ') + *encoded;
                for (std::size_t r = start_row + 1; r < start_row + num_rows; ++r) {
                    lines[r] = "";
                }
            } else if (protocol == InlineImageProtocol::ITerm2) {
                auto encoded = detail::encode_terminal_image(protocol, term_img, handle);
                if (!encoded) continue;
                for (std::size_t r = start_row; r < start_row + num_rows - 1; ++r) {
                    lines[r] = "";
                }
                if (num_rows > 1) {
                    lines[start_row + num_rows - 1] = std::format("\x1b[{}A\r", num_rows - 1) +
                                                      std::string(start_col, ' ') + *encoded +
                                                      std::format("\x1b[{}B\r", num_rows - 1);
                } else {
                    lines[start_row] = std::string(start_col, ' ') + *encoded;
                }
            }
        }
    }

    std::unordered_set<std::uint64_t> active_resources;
    if (protocol != InlineImageProtocol::None) {
        for (const auto& img : images) {
            if (img.resource_id > 0) active_resources.insert(img.resource_id);
        }
    }
    std::erase_if(image_placements_, [&](const auto& pair) { return !active_resources.contains(pair.first); });
}

} // namespace cch::tui::detail
