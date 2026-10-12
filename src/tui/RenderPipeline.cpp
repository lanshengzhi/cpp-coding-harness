#include "RenderPipeline.hpp"

#include <cch/tui/TerminalImage.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/src/tui-main-screen.ts
// Sidecar fork B relative line flow.
// No DSR (\x1b[6n]) query, no dock partition, no absolute scroll margins.
// First frame flows downward from shell cursor; differential updates use
// computeLineDiff and ANSI relative cursor movements.

namespace cch::tui::detail {

RenderPipeline::RenderPipeline(Terminal& terminal) : terminal_(terminal) {}

void RenderPipeline::start(TerminalDimensions) noexcept {
    first_render_ = true;
    hardware_cursor_row_ = 0;
    max_lines_rendered_ = 0;
    previous_viewport_top_ = 0;
    previous_lines_.clear();
    previous_raw_lines_.clear();
    previous_dimensions_.reset();
}

void RenderPipeline::begin_frame() noexcept { frame_prepare_call_count_ = 0; }

void RenderPipeline::note_prepared_line() noexcept { ++frame_prepare_call_count_; }

std::size_t RenderPipeline::frame_prepare_call_count() const noexcept { return frame_prepare_call_count_; }

support::ExpectedVoid RenderPipeline::stop(std::optional<CursorPosition>) {
    const auto image_result = remove_active_images();
    support::ExpectedVoid exit_result;
    if (!previous_lines_.empty()) {
        std::string buffer;
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

    active_images_.clear();
    first_render_ = true;
    previous_lines_.clear();
    previous_raw_lines_.clear();
    previous_dimensions_.reset();
    previous_viewport_top_ = 0;
    hardware_cursor_row_ = 0;
    max_lines_rendered_ = 0;
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
    previous_lines_.clear();
    previous_raw_lines_.clear();
    previous_viewport_top_ = 0;
    hardware_cursor_row_ = 0;
    max_lines_rendered_ = 0;
    previous_dimensions_.reset();
    first_render_ = true;
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
        (void)terminal_.write(buffer);
    }
    hardware_cursor_row_ = target_row;
}

support::ExpectedVoid RenderPipeline::render(RenderResult materialized,
        TerminalDimensions dimensions,
        TerminalCapabilities capabilities,
        bool compose_overlays,
        bool has_overlay_entries,
        bool clear_on_shrink,
        std::optional<CursorPosition> cursor_location) {
    auto& new_lines = materialized.lines;
    auto desired_images = std::move(materialized.images);

    const auto initial_first_render = first_render_;
    const auto initial_lines = previous_lines_;
    const auto initial_raw_lines = previous_raw_lines_;
    const auto initial_dimensions = previous_dimensions_;
    const auto initial_max_lines = max_lines_rendered_;
    const auto initial_viewport_top = previous_viewport_top_;
    const auto initial_cursor_row = hardware_cursor_row_;
    const auto initial_active_images = active_images_;

    const auto rollback = [&] {
        first_render_ = initial_first_render;
        previous_lines_ = initial_lines;
        previous_raw_lines_ = initial_raw_lines;
        previous_dimensions_ = initial_dimensions;
        max_lines_rendered_ = initial_max_lines;
        previous_viewport_top_ = initial_viewport_top;
        hardware_cursor_row_ = initial_cursor_row;
        active_images_ = initial_active_images;
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
        for (auto& line : lines) {
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
        for (std::size_t index = 0; index < new_lines.size(); ++index) {
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

    auto full_render = [&](bool clear) -> support::ExpectedVoid {
        if (supports_sync) {
            if (auto begin_result = terminal_.begin_synchronized_update(); !begin_result) {
                rollback();
                return std::unexpected(begin_result.error());
            }
        }
        if (clear) {
            if (auto removed = remove_active_images(); !removed) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                rollback();
                return std::unexpected(removed.error());
            }
            if (auto cleared = terminal_.clear_screen(); !cleared) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                rollback();
                return std::unexpected(cleared.error());
            }
            hardware_cursor_row_ = 0;
            previous_viewport_top_ = 0;
            hardware_cursor_row = 0;
            prev_viewport_top = 0;
            viewport_top = 0;
        }
        for (std::size_t i = 0; i < new_lines.size(); ++i) {
            std::string line_to_write;
            if (i > 0) line_to_write += "\r\n";
            line_to_write += new_lines[i];
            if (auto result = terminal_.write(line_to_write); !result) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                rollback();
                return std::unexpected(result.error());
            }
        }
        if (supports_sync) {
            if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
                rollback();
                return std::unexpected(end_result.error());
            }
        }
        hardware_cursor_row_ = new_lines.empty() ? 0 : new_lines.size() - 1;
        if (clear) {
            max_lines_rendered_ = new_lines.size();
        } else {
            max_lines_rendered_ = std::max(max_lines_rendered_, new_lines.size());
        }
        const auto buffer_length = std::max(height, new_lines.size());
        previous_viewport_top_ = buffer_length > height ? buffer_length - height : 0;

        position_hardware_cursor(cursor_location, new_lines.size());
        previous_lines_ = std::move(new_lines);
        previous_raw_lines_ = std::move(next_raw_lines);
        previous_dimensions_ = dimensions;
        first_render_ = false;
        return {};
    };

    const auto update_images = [&]() -> support::ExpectedVoid {
        if (auto stale_result = remove_stale_images(desired_images); !stale_result) {
            rollback();
            return stale_result;
        }
        if (auto place_result = place_images(desired_images); !place_result) {
            rollback();
            return place_result;
        }
        return {};
    };

    // 1. First render
    if ((first_render || previous_lines_.empty()) && !width_changed && !height_changed) {
        if (auto result = full_render(false); !result) return result;
        return update_images();
    }

    // 2. Width changed
    if (width_changed) {
        if (auto result = full_render(true); !result) return result;
        return update_images();
    }

    // 3. Height changed
    if (height_changed) {
        if (auto result = full_render(true); !result) return result;
        return update_images();
    }

    // 4. clearOnShrink
    if (clear_on_shrink && new_lines.size() < max_lines_rendered_ && !has_overlay_entries) {
        if (auto result = full_render(true); !result) return result;
        return update_images();
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
    const bool appended_lines = new_lines.size() > previous_lines_.size();
    if (appended_lines) {
        if (first_changed == -1) first_changed = static_cast<std::ptrdiff_t>(previous_lines_.size());
        last_changed = static_cast<std::ptrdiff_t>(new_lines.size() - 1);
    }
    const bool append_start =
            appended_lines && first_changed == static_cast<std::ptrdiff_t>(previous_lines_.size()) && first_changed > 0;

    // 6. No changes - only reposition hardware cursor (no synchronized output)
    if (first_changed == -1) {
        position_hardware_cursor(cursor_location, new_lines.size());
        previous_viewport_top_ = prev_viewport_top;
        previous_dimensions_ = dimensions;
        return update_images();
    }

    // 7. All changes are in deleted lines
    if (first_changed >= static_cast<std::ptrdiff_t>(new_lines.size())) {
        if (previous_lines_.size() > new_lines.size()) {
            const std::size_t target_row = new_lines.empty() ? 0 : new_lines.size() - 1;
            if (target_row < prev_viewport_top) {
                if (auto result = full_render(true); !result) return result;
                return update_images();
            }
            const auto line_diff = compute_line_diff(target_row);
            const auto extra_lines = previous_lines_.size() - new_lines.size();
            if (extra_lines > height) {
                if (auto result = full_render(true); !result) return result;
                return update_images();
            }
            std::string buffer;
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
                    return std::unexpected(begin_result.error());
                }
            }
            if (auto result = terminal_.write(buffer); !result) {
                if (supports_sync) (void)terminal_.end_synchronized_update();
                rollback();
                return std::unexpected(result.error());
            }
            if (supports_sync) {
                if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
                    rollback();
                    return std::unexpected(end_result.error());
                }
            }
            hardware_cursor_row_ = target_row;
        }
        position_hardware_cursor(cursor_location, new_lines.size());
        previous_lines_ = std::move(new_lines);
        previous_raw_lines_ = std::move(next_raw_lines);
        previous_dimensions_ = dimensions;
        previous_viewport_top_ = prev_viewport_top;
        return update_images();
    }

    // 8. firstChanged < prevViewportTop
    if (first_changed < static_cast<std::ptrdiff_t>(prev_viewport_top)) {
        if (auto result = full_render(true); !result) return result;
        return update_images();
    }

    // 9. Differential write
    if (supports_sync) {
        if (auto begin_result = terminal_.begin_synchronized_update(); !begin_result) {
            rollback();
            return std::unexpected(begin_result.error());
        }
    }

    const std::size_t prev_viewport_bottom = prev_viewport_top + height - 1;
    const std::size_t move_target_row =
            append_start ? static_cast<std::size_t>(first_changed - 1) : static_cast<std::size_t>(first_changed);

    std::string buffer;

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
        buffer += "\x1b[2K";
        buffer += new_lines[i];
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

    if (auto result = terminal_.write(buffer); !result) {
        if (supports_sync) (void)terminal_.end_synchronized_update();
        rollback();
        return std::unexpected(result.error());
    }

    if (supports_sync) {
        if (auto end_result = terminal_.end_synchronized_update(); !end_result) {
            rollback();
            return std::unexpected(end_result.error());
        }
    }

    hardware_cursor_row_ = final_cursor_row;
    max_lines_rendered_ = std::max(max_lines_rendered_, new_lines.size());
    previous_viewport_top_ =
            std::max(prev_viewport_top, final_cursor_row >= height ? final_cursor_row - height + 1 : std::size_t{0});

    position_hardware_cursor(cursor_location, new_lines.size());
    previous_lines_ = std::move(new_lines);
    previous_raw_lines_ = std::move(next_raw_lines);
    previous_dimensions_ = dimensions;
    return update_images();
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

support::ExpectedVoid RenderPipeline::remove_stale_images(
        const std::vector<InlineImageRenderRegion>& desired_images) {
    std::size_t index = 0;
    while (index < active_images_.size()) {
        const auto& active = active_images_[index];
        const auto retained = std::find_if(desired_images.begin(), desired_images.end(), [&](const auto& desired) {
            return active.resource_id == desired.resource_id && active.region == desired.region;
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

} // namespace cch::tui::detail
