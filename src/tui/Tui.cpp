#include <cch/tui/Tui.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

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
            // Hide cursor was already called in start().
            for (std::size_t row = 0; row < new_lines.size(); ++row) {
                if (auto result = write_line(terminal_, row, new_lines[row]); !result) {
                    return std::unexpected(result.error());
                }
            }
            // Place cursor at the end of written content so the terminal advances
            // its scrollback past the rendered content.
            if (auto result = terminal_.set_cursor(
                    CursorPosition{.column = 0, .row = new_lines.empty() ? 0U : new_lines.size() - 1});
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

util::ExpectedVoid Tui::set_focus(Component* component) {
    if (component != nullptr && !owns(component)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI focus target is not attached to this root"));
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
    auto* input_handler = dynamic_cast<InputHandler*>(focused_);
    if (input_handler == nullptr) return;
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key != nullptr && key->type == KeyEventType::Release && !input_handler->accepts_key_releases()) {
        return;
    }
    input_handler->handle_input(event);
}

void Tui::handle_resize(TerminalDimensions) {
    invalidate();
}

} // namespace cch::tui
