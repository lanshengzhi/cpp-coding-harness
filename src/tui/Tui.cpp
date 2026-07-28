#include <cch/tui/Tui.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/TerminalImage.hpp"
#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

constexpr std::size_t kInputDecodeChunkBytes = 4096;

[[nodiscard]] std::string describe_error(const util::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] util::Error startup_rollback_error(
    const util::Error& startup,
    const util::Error& rollback) {
    return util::make_error(
        startup.code,
        "TUI startup failed and terminal restoration was incomplete",
        std::format(
            "startup: {}; restoration: {}",
            describe_error(startup),
            describe_error(rollback)));
}

/// Write a single full-width line to the terminal at the given row.
[[nodiscard]] util::ExpectedVoid write_line(
    Terminal& terminal,
    std::size_t row,
    std::string_view line) {
    if (auto result = terminal.set_cursor(CursorPosition{.column = 0, .row = row}); !result) {
        return std::unexpected(result.error());
    }
    return terminal.write(line);
}

/// Clear a row of the terminal by writing spaces at the full terminal width.
[[nodiscard]] util::ExpectedVoid clear_row(Terminal& terminal, std::size_t row, std::size_t columns) {
    if (auto result = terminal.set_cursor(CursorPosition{.column = 0, .row = row}); !result) {
        return std::unexpected(result.error());
    }
    // Write spaces to fill the row, then move cursor back so the next write overwrites cleanly
    return terminal.write(std::string(columns, ' '));
}

[[nodiscard]] util::Expected<std::string> line_suffix_from_column(
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

[[nodiscard]] util::Expected<std::string> replace_line_region(
    std::string_view line,
    std::string_view replacement,
    std::size_t column,
    std::size_t columns,
    std::size_t total_width) {
    auto prefix = detail::truncate_text(line, column, "", true);
    if (!prefix) return std::unexpected(prefix.error());
    auto bounded_replacement = detail::truncate_text(replacement, columns, "", true);
    if (!bounded_replacement) return std::unexpected(bounded_replacement.error());
    auto suffix = line_suffix_from_column(line, column + columns);
    if (!suffix) return std::unexpected(suffix.error());
    return detail::truncate_text(
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

util::Expected<std::reference_wrapper<Component>> Tui::add_child(std::unique_ptr<Component> component) {
    std::lock_guard lock(mutex_);
    return detail::attach_child(children_, std::move(component), "");
}

util::Expected<std::reference_wrapper<Overlay>> Tui::add_overlay(std::unique_ptr<Overlay> overlay) {
    std::lock_guard lock(mutex_);
    if (!overlay) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI cannot attach a null Overlay"));
    }
    auto& ref = *overlay;
    overlays_.push_back(std::move(overlay));
    return ref;
}

util::ExpectedVoid Tui::remove_overlay(Overlay* overlay) {
    std::lock_guard lock(mutex_);
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    const bool was_focused = focused_ == static_cast<Component*>(overlay);
    auto* return_focus = overlay_return_focus(overlay);
    if (was_focused) apply_focus(nullptr);
    forget_overlay_focus(overlay, return_focus);

    const auto before = overlays_.size();
    std::erase_if(overlays_, [overlay](const auto& ptr) { return ptr.get() == overlay; });
    if (overlays_.size() == before) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Overlay not found in TUI"));
    }

    if (was_focused) {
        if (focus_target_available(return_focus)) apply_focus(return_focus);
        else fallback_focus();
    }
    return {};
}

util::ExpectedVoid Tui::hide_overlay(Overlay* overlay) {
    std::lock_guard lock(mutex_);
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
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

util::ExpectedVoid Tui::restore_overlay(Overlay* overlay) {
    std::lock_guard lock(mutex_);
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    overlay->set_visible(true);
    invalidate();
    return {};
}

util::ExpectedVoid Tui::start() {
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
    previous_lines_.clear();
    previous_dimensions_ = terminal_.dimensions();
    return {};
}

util::ExpectedVoid Tui::stop() {
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

util::ExpectedVoid Tui::render() {
    std::lock_guard lock(mutex_);
    if (!started_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI must be started before rendering"));
    }

    const auto dimensions = terminal_.dimensions();
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI requires positive terminal dimensions"));
    }

    const auto capabilities = terminal_.capabilities();
    auto rendered = render_children(dimensions);
    if (!rendered) return std::unexpected(rendered.error());
    auto materialized = materialize_images(
        std::move(*rendered),
        capabilities,
        dimensions.columns,
        dimensions.rows);
    if (auto overlay_result = composite_overlays(dimensions, capabilities, materialized);
        !overlay_result) {
        return std::unexpected(overlay_result.error());
    }
    auto& new_lines = materialized.lines;
    auto desired_images = std::move(materialized.images);

    const auto visible_lines = std::min(new_lines.size(), dimensions.rows);
    new_lines.resize(visible_lines);
    for (auto& line : new_lines) {
        if (line.size() < dimensions.columns) {
            line.append(dimensions.columns - line.size(), ' ');
        }
    }

    const auto width_changed = dimensions.columns != previous_dimensions_.columns;
    const auto first_render = first_render_;

    const auto supports_sync = capabilities.synchronized_output;

    // Begin synchronized update if supported
    if (supports_sync) {
        if (auto result = terminal_.begin_synchronized_update(); !result) {
            return std::unexpected(result.error());
        }
    }

    auto render_result = [&]() -> util::ExpectedVoid {
        if (first_render) {
            // First render: write full viewport content without clearing scrollback.
            for (std::size_t row = 0; row < new_lines.size(); ++row) {
                if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                    return std::unexpected(result.error());
                }
            }
            // Place cursor at the end of written content so the terminal advances
            // its scrollback past the rendered content.
            if (auto result = terminal_.set_cursor(
                    CursorPosition{.column = 0, .row = new_lines.empty() ? 0U : std::min(new_lines.size(), dimensions.rows) - 1});
                !result) {
                return std::unexpected(result.error());
            }
            return {};
        }

        if (width_changed) {
            // Width change: full redraw with clear screen to reflow all content.
            if (auto result = remove_active_images(); !result) {
                return std::unexpected(result.error());
            }
            if (auto result = terminal_.clear_screen(); !result) {
                return std::unexpected(result.error());
            }
            for (std::size_t row = 0; row < new_lines.size(); ++row) {
                if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                    return std::unexpected(result.error());
                }
            }
            return {};
        }

        // Differential update: find the first changed visible line.
        const auto min_previous = std::min(previous_lines_.size(), new_lines.size());
        std::size_t first_diff = 0;
        while (first_diff < min_previous && previous_lines_[first_diff] == new_lines[first_diff]) {
            ++first_diff;
        }

        if (first_diff == 0 && !previous_lines_.empty()) {
            // Changes above the tracked viewport: full redraw.
            if (auto result = remove_active_images(); !result) {
                return std::unexpected(result.error());
            }
            if (auto result = terminal_.clear_screen(); !result) {
                return std::unexpected(result.error());
            }
            for (std::size_t row = 0; row < new_lines.size(); ++row) {
                if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                    return std::unexpected(result.error());
                }
            }
            return {};
        }

        // Write changed and new lines starting at first_diff.
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

        // If content has shrunk, clear stale rows below the new content.
        if (new_lines.size() < previous_lines_.size()) {
            for (std::size_t row = new_lines.size(); row < previous_lines_.size(); ++row) {
                if (row >= dimensions.rows) break;
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

    // Position IME cursor based on focused component
    if (render_result) {
        auto cursor_loc = resolve_cursor_location();
        if (cursor_loc) {
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

util::Expected<RenderResult> Tui::render_children(TerminalDimensions dimensions) {
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

util::ExpectedVoid Tui::composite_overlays(
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
            content_width = std::max(content_width, detail::visible_width(line));
        }
        const auto content_height = materialized.lines.size();
        const auto [offset_col, offset_row] = overlay->layout_position(
            dimensions.columns, dimensions.rows, content_width, content_height);
        const auto& margins = overlay->options().margins;
        const auto final_col = offset_col + margins.left;
        const auto final_row = offset_row + margins.top;
        if (final_col >= dimensions.columns || final_row >= dimensions.rows) continue;

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
            composited_width = std::max(composited_width, detail::visible_width(line));
        }
        const auto overlay_width = std::min(composited_width, dimensions.columns - final_col);
        for (std::size_t row = 0; row < content_height; ++row) {
            const auto target_row = final_row + row;
            if (target_row >= dimensions.rows || overlay_width == 0) break;
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
            image.region.row += final_row;
            const auto fits_columns = image.region.column < dimensions.columns &&
                image.region.columns <= dimensions.columns - image.region.column;
            const auto fits_rows = image.region.row < dimensions.rows &&
                image.region.rows <= dimensions.rows - image.region.row;
            if (fits_columns && fits_rows) output.images.push_back(std::move(image));
        }
    }

    return {};
}

util::ExpectedVoid Tui::remove_active_images() {
    while (!active_images_.empty()) {
        const auto image = active_images_.back();
        if (auto removed = terminal_.remove_image(image.handle, image.region); !removed) {
            return std::unexpected(removed.error());
        }
        active_images_.pop_back();
    }
    return {};
}

util::ExpectedVoid Tui::remove_images_intersecting(const CellRegion& region) {
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

util::ExpectedVoid Tui::remove_stale_images(
    const std::vector<InlineImageRenderRegion>& desired_images) {
    std::size_t index = 0;
    while (index < active_images_.size()) {
        const auto& active = active_images_[index];
        const auto retained = std::find_if(
            desired_images.begin(),
            desired_images.end(),
            [&](const auto& desired) {
                return active.resource_id == desired.resource_id &&
                    active.revision == desired.revision &&
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

util::ExpectedVoid Tui::place_images(
    const std::vector<InlineImageRenderRegion>& desired_images) {
    for (const auto& image : desired_images) {
        const auto active = std::find_if(
            active_images_.begin(),
            active_images_.end(),
            [&](const auto& candidate) {
                return candidate.resource_id == image.resource_id &&
                    candidate.revision == image.revision &&
                    candidate.region == image.region;
            });
        if (active != active_images_.end()) continue;

        std::optional<std::string_view> filename;
        if (image.filename) filename = *image.filename;
        const TerminalImage terminal_image{
            .encoded_data = image.encoded_data,
            .mime_type = image.mime_type,
            .filename = filename,
            .pixel_width = image.pixel_width,
            .pixel_height = image.pixel_height,
            .resource_id = image.resource_id,
            .revision = image.revision,
            .region = image.region,
        };
        auto placed = terminal_.place_image(terminal_image);
        if (!placed) return std::unexpected(placed.error());
        active_images_.push_back({
            .handle = *placed,
            .region = image.region,
            .resource_id = image.resource_id,
            .revision = image.revision,
        });
    }
    return {};
}

util::ExpectedVoid Tui::set_focus(Component* component) {
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
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "TUI focus target is not attached to this root"));
        }
    }
    if (component != nullptr && dynamic_cast<Focusable*>(component) == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
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
