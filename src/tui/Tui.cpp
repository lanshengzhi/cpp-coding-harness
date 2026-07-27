#include <cch/tui/Tui.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

constexpr std::size_t kInputDecodeChunkBytes = 4096;

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
    return detail::attach_child(children_, std::move(component), "");
}

util::Expected<std::reference_wrapper<Overlay>> Tui::add_overlay(std::unique_ptr<Overlay> overlay) {
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
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    // Remember if this overlay was focused before we remove it.
    const bool was_focused = (focused_ == static_cast<Component*>(overlay));

    const auto before = overlays_.size();
    std::erase_if(overlays_, [overlay](const auto& ptr) { return ptr.get() == overlay; });
    if (overlays_.size() == before) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Overlay not found in TUI"));
    }

    // Fallback focus after removal so we don't target the removed overlay.
    if (was_focused) {
        focused_ = nullptr;
        fallback_focus();
    }
    return {};
}

util::ExpectedVoid Tui::hide_overlay(Overlay* overlay) {
    if (overlay == nullptr) return {};
    if (!owns_overlay(overlay)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Overlay is not attached to this TUI"));
    }

    overlay->set_visible(false);

    // If the hidden overlay was focused, find a fallback
    auto* as_component = static_cast<Component*>(overlay);
    if (focused_ == as_component) {
        focused_ = nullptr;
        fallback_focus();
    }

    invalidate();
    return {};
}

util::ExpectedVoid Tui::restore_overlay(Overlay* overlay) {
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
        (void)terminal_.stop();
        started_ = false;
        return std::unexpected(result.error());
    }

    first_render_ = true;
    pending_render_ = false;
    previous_lines_.clear();
    previous_dimensions_ = terminal_.dimensions();
    return {};
}

util::ExpectedVoid Tui::stop() {
    if (!started_) {
        return {};
    }

    // Unfocus everything before stopping
    if (auto* focusable = dynamic_cast<Focusable*>(focused_)) {
        focusable->set_focused(false);
    }
    focused_ = nullptr;

    const auto cursor_result = terminal_.set_cursor_visible(true);
    const auto stop_result = terminal_.stop();
    input_decoder_->reset();
    started_ = false;
    first_render_ = true;
    pending_render_ = false;

    if (!cursor_result) {
        return std::unexpected(cursor_result.error());
    }
    if (!stop_result) {
        return std::unexpected(stop_result.error());
    }
    return {};
}

util::ExpectedVoid Tui::render() {
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

    // Build the new set of rendered lines from children
    auto rendered = render_children(dimensions);
    if (!rendered) return std::unexpected(rendered.error());
    auto& new_lines = *rendered;

    // Clamp visible lines to terminal height
    const auto visible_lines = std::min(new_lines.size(), dimensions.rows);
    new_lines.resize(visible_lines);
    for (auto& line : new_lines) {
        if (line.size() < dimensions.columns) {
            line.append(dimensions.columns - line.size(), ' ');
        }
    }

    const auto width_changed = dimensions.columns != previous_dimensions_.columns;
    const auto first_render = first_render_;

    const auto supports_sync = terminal_.capabilities().synchronized_output;

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
            if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                return std::unexpected(result.error());
            }
        }

        // If content has shrunk, clear stale rows below the new content.
        if (new_lines.size() < previous_lines_.size()) {
            for (std::size_t row = new_lines.size(); row < previous_lines_.size(); ++row) {
                if (row >= dimensions.rows) break;
                if (auto result = clear_row(terminal_, row, dimensions.columns); !result) {
                    return std::unexpected(result.error());
                }
            }
        }
        return {};
    }();

    // Render overlays on top of base content
    if (render_result) {
        if (auto overlay_result = render_overlays(dimensions); !overlay_result) {
            render_result = std::unexpected(overlay_result.error());
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

util::Expected<std::vector<std::string>> Tui::render_children(TerminalDimensions dimensions) {
    std::vector<std::string> lines;
    for (const auto& child : children_) {
        if (auto* viewport_aware = dynamic_cast<ViewportAware*>(child.get())) {
            viewport_aware->set_available_height(dimensions.rows);
        }
        auto rendered = child->render(dimensions.columns);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        for (auto& line : *rendered) {
            auto prepared = detail::prepare_rendered_line(line, dimensions.columns);
            if (!prepared) return std::unexpected(prepared.error());
            lines.push_back(std::move(*prepared));
        }
    }
    return lines;
}

util::ExpectedVoid Tui::render_overlays(TerminalDimensions dimensions) {
    auto visible_overlays = sorted_visible_overlays(overlays_, dimensions);

    for (auto* overlay : visible_overlays) {
        // Render the overlay content
        auto rendered = overlay->render(dimensions.columns);
        if (!rendered) return std::unexpected(rendered.error());

        if (rendered->empty()) continue;

        // Compute content dimensions
        std::size_t content_width = 0;
        for (const auto& line : *rendered) {
            content_width = std::max(content_width, detail::visible_width(line));
        }
        const auto content_height = rendered->size();

        // Compute layout position
        const auto [offset_col, offset_row] = overlay->layout_position(
            dimensions.columns, dimensions.rows, content_width, content_height);

        // Apply margins
        const auto& margins = overlay->options().margins;
        const auto final_col = offset_col + margins.left;
        const auto final_row = offset_row + margins.top;

        // Write each line at its position, clipping to viewport
        for (std::size_t row = 0; row < content_height; ++row) {
            const auto target_row = final_row + row;
            if (target_row >= dimensions.rows) break;

            const auto& line = (*rendered)[row];
            // Clip horizontally
            const auto target_col = std::min(final_col, dimensions.columns - 1);
            const auto visible_width = std::min(
                static_cast<std::size_t>(line.size()),
                dimensions.columns - target_col);

            if (visible_width == 0) continue;

            auto display_line = line.substr(0, visible_width);
            if (display_line.size() < dimensions.columns) {
                display_line.append(dimensions.columns - display_line.size(), ' ');
            }

            if (auto result = write_line(terminal_, target_row, display_line); !result) {
                return std::unexpected(result.error());
            }
        }
    }

    return {};
}

util::ExpectedVoid Tui::set_focus(Component* component) {
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

    if (auto* previous = dynamic_cast<Focusable*>(focused_)) {
        previous->set_focused(false);
    }
    focused_ = component;
    if (auto* next = dynamic_cast<Focusable*>(focused_)) {
        next->set_focused(true);
    }
    return {};
}

void Tui::invalidate() {
    pending_render_ = true;
    for (const auto& child : children_) {
        child->invalidate();
    }
    for (const auto& overlay : overlays_) {
        overlay->invalidate();
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
    invalidate();
}

void Tui::fallback_focus() {
    if (auto* target = find_focusable_target()) {
        auto* as_component = dynamic_cast<Component*>(target);
        if (as_component != focused_) {
            if (auto* previous = dynamic_cast<Focusable*>(focused_)) {
                previous->set_focused(false);
            }
            focused_ = as_component;
            target->set_focused(true);
        }
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
