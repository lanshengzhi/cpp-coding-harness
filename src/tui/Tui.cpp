#include <cch/tui/Tui.hpp>

#include "tui/UnicodeWidth.hpp"

#include <format>
#include <utility>

namespace cch::tui {

Tui::Tui(Terminal& terminal)
    : terminal_(terminal) {}

Tui::~Tui() {
    (void)stop();
}

util::Expected<std::reference_wrapper<Component>> Tui::add_child(std::unique_ptr<Component> component) {
    if (!component) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI cannot attach a null Component"));
    }

    auto& child = *component;
    children_.push_back(std::move(component));
    return child;
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
    return {};
}

util::ExpectedVoid Tui::stop() {
    if (!started_) {
        return {};
    }

    const auto cursor_result = terminal_.set_cursor_visible(true);
    const auto stop_result = terminal_.stop();
    started_ = false;

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

    std::vector<std::string> lines;
    for (const auto& child : children_) {
        auto rendered = child->render(dimensions.columns);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        for (auto& line : *rendered) {
            const auto line_vis = detail::visible_width(line);
            if (line_vis > static_cast<int>(dimensions.columns)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    "TUI component rendered a line wider than its width bound",
                    std::format(
                        "line width {} exceeds visible width {}",
                        line_vis,
                        dimensions.columns)));
            }
            lines.push_back(std::move(line));
        }
    }

    if (auto result = terminal_.clear_screen(); !result) {
        return std::unexpected(result.error());
    }

    const auto rows_to_render = std::min(lines.size(), dimensions.rows);
    for (std::size_t row = 0; row < rows_to_render; ++row) {
        if (auto result = terminal_.set_cursor(CursorPosition{.column = 0, .row = row}); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = terminal_.write(lines[row]); !result) {
            return std::unexpected(result.error());
        }
    }
    if (lines.empty()) {
        if (auto result = terminal_.set_cursor(CursorPosition{}); !result) {
            return std::unexpected(result.error());
        }
    }
    return {};
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
    if (auto* input_handler = dynamic_cast<InputHandler*>(focused_)) {
        input_handler->handle_input(input);
    }
}

void Tui::handle_resize(TerminalDimensions) {
    invalidate();
}

} // namespace cch::tui
