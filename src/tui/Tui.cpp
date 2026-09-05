#include <cch/tui/Tui.hpp>

#include <cch/tui/TerminalImage.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/OverlayCompositor.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
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

/// Write a single full-width line directly to the terminal's reserved bottom dock region.
[[nodiscard]] support::ExpectedVoid write_dock_line(
    Terminal& terminal,
    std::size_t dock_row,
    std::string_view line) {
    if (auto result = terminal.set_dock_cursor(dock_row, 0); !result) {
        return std::unexpected(result.error());
    }
    return terminal.write(line);
}

} // namespace

Tui::Tui(Terminal& terminal)
    : terminal_(terminal),
      stream_decoder_(std::make_unique<detail::TerminalStreamDecoder>()),
      compositor_(std::make_unique<detail::OverlayCompositor>()) {}

Tui::~Tui() {
    (void)stop();
}

support::Expected<std::reference_wrapper<Component>> Tui::add_child(std::unique_ptr<Component> component) {
    return detail::attach_child(children_, std::move(component), "");
}

support::Expected<std::reference_wrapper<Overlay>> Tui::add_overlay(std::unique_ptr<Overlay> overlay) {
    return compositor_->add_overlay(std::move(overlay));
}

support::ExpectedVoid Tui::remove_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    const bool was_focused = focused_ == static_cast<Component*>(overlay);
    auto* return_focus = compositor_->return_focus(overlay);
    if (was_focused) apply_focus(nullptr);
    compositor_->forget_focus(overlay, return_focus);

    if (auto removed = compositor_->remove(overlay); !removed) {
        return std::unexpected(removed.error());
    }

    if (was_focused) {
        if (focus_target_available(return_focus)) apply_focus(return_focus);
        else fallback_focus();
    }
    return {};
}

support::ExpectedVoid Tui::hide_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    auto* return_focus = compositor_->return_focus(overlay);
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
    if (overlay == nullptr) return {};
    if (!compositor_->owns(overlay)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    overlay->set_visible(true);
    invalidate();
    return {};
}

support::ExpectedVoid Tui::start() {
    if (started_) {
        return {};
    }

    if (auto result = terminal_.start(
            [this](std::string input) -> support::ExpectedVoid {
                handle_input(std::move(input));
                return {};
            },
            [this](TerminalDimensions dimensions) -> support::ExpectedVoid {
                handle_resize(dimensions);
                return {};
            });
        !result) {
        return std::unexpected(result.error());
    }
    started_ = true;

    if (auto result = terminal_.set_cursor_visible(false); !result) {
        started_ = false;
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
    if (!started_) return {};

    started_ = false;
    if (auto* focusable = dynamic_cast<Focusable*>(focused_)) {
        focusable->set_focused(false);
    }
    focused_ = nullptr;
    compositor_->clear_focus_history();

    const auto image_result = remove_active_images();
    (void)terminal_.reset_scroll_margins();
    previous_dock_lines_.clear();
    // pi TuiMainScreen::beforeTerminalStop: move the cursor below the composed
    // buffer's last line and end the line, so the shell prompt resumes under
    // the transcript instead of overwriting its last line. Under the anchored
    // absolute flow (ADR 0041) the relative movement is one absolute
    // set_cursor one row past the buffer; rows past the visible bottom flow
    // and scroll through the terminal mapping.
    support::ExpectedVoid exit_result;
    if (!previous_lines_.empty()) {
        if (auto written = terminal_.write(" "); !written) {
            exit_result = std::unexpected(written.error());
        } else if (auto positioned = terminal_.set_cursor(
                       CursorPosition{.column = 0, .row = previous_lines_.size()});
                   !positioned) {
            exit_result = std::unexpected(positioned.error());
        } else if (auto newline = terminal_.write("\r\n"); !newline) {
            exit_result = std::unexpected(newline.error());
        }
    }
    const auto cursor_result = terminal_.set_cursor_visible(true);
    active_images_.clear();
    stream_decoder_->reset();
    first_render_ = true;
    pending_render_ = false;

    const auto stop_result = terminal_.stop();

    if (!image_result) return std::unexpected(image_result.error());
    if (!exit_result) return std::unexpected(exit_result.error());
    if (!cursor_result) return std::unexpected(cursor_result.error());
    if (!stop_result) return std::unexpected(stop_result.error());
    return {};
}

support::ExpectedVoid Tui::clear_screen() {
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
    (void)terminal_.reset_scroll_margins();
    previous_dock_lines_.clear();
    previous_lines_.clear();
    previous_dimensions_ = {};
    viewport_top_ = 0;
    first_render_ = true;
    return {};
}

support::ExpectedVoid Tui::render() {
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
    auto materialized = detail::OverlayCompositor::materialize_images(
        std::move(*rendered),
        capabilities,
        dimensions.columns,
        std::numeric_limits<std::size_t>::max());
    if (auto overlay_result = compositor_->composite(dimensions, capabilities, materialized);
        !overlay_result) {
        return std::unexpected(overlay_result.error());
    }
    auto& new_lines = materialized.lines;
    auto desired_images = std::move(materialized.images);
    auto& new_dock_lines = materialized.dock_lines;
    const bool has_dock = !new_dock_lines.empty() || materialized.viewport_height.has_value();
    const std::size_t dock_height = new_dock_lines.size();
    const std::size_t viewport_height = materialized.viewport_height.value_or(
        dimensions.rows > dock_height ? dimensions.rows - dock_height : 0);

    // The full composed buffer is written to the terminal's main screen with
    // no viewport clipping; overflow advances into the terminal's native
    // scrollback (pi TuiMainScreen). Lines are padded so the first-diff
    // comparison is byte-stable across renders.
    for (auto& line : new_lines) {
        if (line.size() < dimensions.columns) {
            line.append(dimensions.columns - line.size(), ' ');
        }
    }
    if (has_dock) {
        for (auto& line : new_dock_lines) {
            if (line.size() < dimensions.columns) {
                line.append(dimensions.columns - line.size(), ' ');
            }
        }
    }

    const auto width_changed = dimensions.columns != previous_dimensions_.columns;
    const auto height_changed = dimensions.rows != previous_dimensions_.rows;
    const auto first_render = first_render_;

    const auto supports_sync = capabilities.synchronized_output;
    const auto initial_viewport_top = viewport_top_;
    const auto initial_active_images = active_images_;
    const auto initial_previous_dock_lines = previous_dock_lines_;
    const auto rollback_render_state = [&] {
        viewport_top_ = initial_viewport_top;
        active_images_ = initial_active_images;
        previous_dock_lines_ = initial_previous_dock_lines;
    };

    // Begin synchronized update if supported
    if (supports_sync) {
        if (auto result = terminal_.begin_synchronized_update(); !result) {
            return std::unexpected(result.error());
        }
    }

    auto apply_scroll_margins = [&]() -> support::ExpectedVoid {
        if (has_dock && viewport_height >= 2) {
            return terminal_.set_scroll_margins(0, viewport_height - 1);
        }
        return terminal_.reset_scroll_margins();
    };

    auto write_dock_lines = [&]() -> support::ExpectedVoid {
        for (std::size_t i = 0; i < new_dock_lines.size(); ++i) {
            if (auto result = write_dock_line(terminal_, i, new_dock_lines[i]); !result) {
                return std::unexpected(result.error());
            }
        }
        if (new_dock_lines.size() < previous_dock_lines_.size()) {
            const std::string empty_line(dimensions.columns, ' ');
            for (std::size_t i = new_dock_lines.size(); i < previous_dock_lines_.size(); ++i) {
                if (auto result = write_dock_line(terminal_, i, empty_line); !result) {
                    return std::unexpected(result.error());
                }
            }
        }
        return {};
    };

    auto write_full_buffer = [&]() -> support::ExpectedVoid {
        if (auto margins = apply_scroll_margins(); !margins) {
            return std::unexpected(margins.error());
        }
        for (std::size_t row = 0; row < new_lines.size(); ++row) {
            if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
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
            if (!has_dock) {
                // Leave the cursor at the end of the written content (column 0) so
                // the terminal advances its scrollback past the rendered content.
                if (auto result = terminal_.set_cursor(CursorPosition{
                        .column = 0,
                        .row = new_lines.empty() ? 0U : new_lines.size() - 1});
                    !result) {
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
            return clear_and_rewrite();
        }
        if (auto margins = apply_scroll_margins(); !margins) {
            return std::unexpected(margins.error());
        }


        // pi differential: first-changed-line tracking over the full buffer.
        const auto min_previous = std::min(previous_lines_.size(), new_lines.size());
        std::size_t first_diff = 0;
        while (first_diff < min_previous && previous_lines_[first_diff] == new_lines[first_diff]) {
            ++first_diff;
        }
        const auto viewport_unchanged =
            first_diff == min_previous && previous_lines_.size() == new_lines.size();
        const auto dock_unchanged =
            !has_dock || (previous_dock_lines_ == new_dock_lines);
        if (viewport_unchanged && dock_unchanged) return {};

        if (!viewport_unchanged) {

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
                const auto visible_rows = has_dock ? viewport_height : dimensions.rows;
                const auto stale_end = std::min(
                    previous_lines_.size(),
                    viewport_top_ + visible_rows);
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
    const auto visible_rows = has_dock ? viewport_height : dimensions.rows;
    viewport_top_ = std::max(
        viewport_top_,
        new_lines.size() > visible_rows ? new_lines.size() - visible_rows : 0U);

    // Position IME cursor based on focused component
    if (render_result) {
        auto cursor_loc = resolve_cursor_location();
        if (cursor_loc) {
            if (has_dock) {
                std::size_t dock_row = cursor_loc->row;
                if (dock_row >= viewport_height && dock_row < dimensions.rows) {
                    dock_row -= viewport_height;
                }
                if (dock_row < new_dock_lines.size()) {
                    if (auto cursor_result = terminal_.set_dock_cursor(dock_row, cursor_loc->column); !cursor_result) {
                        render_result = std::unexpected(cursor_result.error());
                    }
                } else {
                    const auto viewport_bottom = viewport_top_ + viewport_height - 1;
                    if (cursor_loc->row < viewport_top_) cursor_loc->row = viewport_top_;
                    else if (cursor_loc->row > viewport_bottom) cursor_loc->row = viewport_bottom;
                    if (auto cursor_result = terminal_.set_cursor(*cursor_loc); !cursor_result) {
                        render_result = std::unexpected(cursor_result.error());
                    }
                }
            } else {
                const auto viewport_bottom = viewport_top_ + dimensions.rows - 1;
                if (cursor_loc->row < viewport_top_) cursor_loc->row = viewport_top_;
                else if (cursor_loc->row > viewport_bottom) cursor_loc->row = viewport_bottom;
                if (auto cursor_result = terminal_.set_cursor(*cursor_loc); !cursor_result) {
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
        rollback_render_state();
        return std::unexpected(render_result.error());
    }

    // Update cached state
    previous_lines_ = std::move(new_lines);
    previous_dock_lines_ = std::move(new_dock_lines);
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
        if (rendered->viewport_height.has_value()) {
            output.viewport_height = rendered->viewport_height;
        }
        for (auto& line : rendered->dock_lines) {
            auto prepared = detail::prepare_rendered_line(line, dimensions.columns);
            if (!prepared) return std::unexpected(prepared.error());
            output.dock_lines.push_back(std::move(*prepared));
        }
    }
    return output;
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
    if (component != nullptr && !owns(component)) {
        // Check if component is inside an overlay
        auto* overlay = dynamic_cast<Overlay*>(component);
        if (overlay == nullptr || !compositor_->owns(overlay)) {
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
        compositor_->remember_focus(overlay, focused_);
    }
    apply_focus(component);
    return {};
}

void Tui::set_render_request_sink(TuiRenderRequestSink sink) {
    render_request_sink_ = std::move(sink);
}

void Tui::invalidate() {
    const bool request_render = started_ && !pending_render_;
    pending_render_ = true;
    for (const auto& child : children_) {
        child->invalidate();
    }
    compositor_->invalidate_all();
    if (request_render && render_request_sink_) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            (void)render_request_sink_();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            // Scheduling notifications cannot make terminal input delivery fail.
        }
#endif
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

void Tui::handle_input(std::string input) {
    if (!started_) return;
    if (input.empty()) {
        // ProcessTerminal applies out-of-band responses before delivering the
        // same bytes here; late response fragments are dropped by its flush.
        for (const auto& event : stream_decoder_->flush().events) dispatch_input(event);
        return;
    }

    for (std::size_t offset = 0; offset < input.size(); offset += kInputDecodeChunkBytes) {
        const auto chunk = std::string_view(input).substr(offset, kInputDecodeChunkBytes);
        for (const auto& event : stream_decoder_->feed(chunk).events) dispatch_input(event);
    }
}

void Tui::dispatch_input(const InputEventVariant& event) {
    // Try overlays first (in reverse z-order, topmost first)
    if (compositor_->dispatch_input(event, previous_dimensions_) == InputAdmissionOutcome::Consumed) return;

    // Fallback to focused base component
    auto* input_handler = dynamic_cast<InputHandler*>(focused_);
    if (input_handler == nullptr) {
        // Try first focusable for initial dispatch
        fallback_focus();
        input_handler = dynamic_cast<InputHandler*>(focused_);
        if (input_handler == nullptr) return;
    }
    static_cast<void>(input_handler->handle_input(event));
}

void Tui::handle_resize(TerminalDimensions) {
    if (!started_) return;
    invalidate();
}

void Tui::apply_focus(Component* component) {
    if (auto* previous = dynamic_cast<Focusable*>(focused_)) previous->set_focused(false);
    focused_ = component;
    if (auto* next = dynamic_cast<Focusable*>(focused_)) next->set_focused(true);
}

bool Tui::focus_target_available(Component* component) const {
    if (component == nullptr) return false;
    if (owns(component)) return dynamic_cast<Focusable*>(component) != nullptr;
    return compositor_->focus_target_available(component, previous_dimensions_);
}

void Tui::fallback_focus() {
    if (auto* target = find_focusable_target()) {
        auto* as_component = dynamic_cast<Component*>(target);
        if (as_component != focused_) apply_focus(as_component);
    }
}

Focusable* Tui::find_focusable_target() {
    // Look in visible overlays first (topmost first)
    if (auto* overlay = compositor_->topmost_focusable(previous_dimensions_)) {
        return overlay;
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
