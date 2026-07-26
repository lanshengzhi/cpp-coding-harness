#include <cch/tui/Container.hpp>

#include "tui/UnicodeWidth.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

// ── Container ────────────────────────────────────────────────────────

util::Expected<std::reference_wrapper<Component>> Container::add_child(
    std::unique_ptr<Component> component)
{
    if (!component) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Container cannot attach a null Component"));
    }

    auto& child = *component;
    children_.push_back(std::move(component));
    return child;
}

util::Expected<std::vector<std::string>> Container::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Container requires a positive visible width"));
    }

    std::vector<std::string> result;
    for (const auto& child : children_) {
        auto lines = child->render(width);
        if (!lines) {
            return std::unexpected(lines.error());
        }
        for (auto& line : *lines) {
            result.push_back(std::move(line));
        }
    }
    return result;
}

void Container::invalidate() {
    for (const auto& child : children_) {
        child->invalidate();
    }
}

// ── Box ──────────────────────────────────────────────────────────────

Box::Box(std::size_t padding_x, std::size_t padding_y,
         std::function<std::string(std::string)> bg_fn)
    : padding_x_(padding_x)
    , padding_y_(padding_y)
    , bg_fn_(std::move(bg_fn)) {}

util::Expected<std::reference_wrapper<Component>> Box::add_child(
    std::unique_ptr<Component> component)
{
    if (!component) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Box cannot attach a null Component"));
    }

    auto& child = *component;
    children_.push_back(std::move(component));
    return child;
}

void Box::clear() {
    children_.clear();
}

util::Expected<std::vector<std::string>> Box::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Box requires a positive visible width"));
    }

    const auto content_width = static_cast<int>(width) - static_cast<int>(padding_x_ * 2);
    if (content_width <= 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Box width is too small for padding",
            "width " + std::to_string(width) +
                " padding_x " + std::to_string(padding_x_)));
    }

    // If no children, return empty even with padding configured
    if (children_.empty()) {
        return std::vector<std::string>{};
    }

    std::vector<std::string> result;

    // Helper to apply background and padding to a full-width line
    auto apply_bg = [&](std::string line) -> std::string {
        if (bg_fn_) {
            return bg_fn_(std::move(line));
        }
        return line;
    };

    // Empty line
    auto make_empty_line = [&]() -> std::string {
        auto line = std::string(width, ' ');
        if (bg_fn_) {
            return bg_fn_(std::move(line));
        }
        return line;
    };

    // Top padding
    for (std::size_t i = 0; i < padding_y_; ++i) {
        result.push_back(make_empty_line());
    }

    // Content lines
    const std::string left_pad(padding_x_, ' ');
    for (const auto& child : children_) {
        auto lines = child->render(static_cast<std::size_t>(content_width));
        if (!lines) {
            return std::unexpected(lines.error());
        }

        for (auto& line : *lines) {
            std::string padded = left_pad + line;
            auto vis = detail::visible_width(padded);
            if (static_cast<int>(width) > vis) {
                padded += std::string(static_cast<std::size_t>(width) - vis, ' ');
            }
            result.push_back(apply_bg(std::move(padded)));
        }
    }

    // Bottom padding
    for (std::size_t i = 0; i < padding_y_; ++i) {
        result.push_back(make_empty_line());
    }

    if (result.empty()) {
        return result;
    }

    return result;
}

void Box::invalidate() {
    for (const auto& child : children_) {
        child->invalidate();
    }
}

// ── Spacer ───────────────────────────────────────────────────────────

Spacer::Spacer(std::size_t lines)
    : lines_(lines) {}

void Spacer::set_lines(std::size_t lines) {
    lines_ = lines;
}

util::Expected<std::vector<std::string>> Spacer::render(std::size_t /*width*/) {
    std::vector<std::string> result;
    result.reserve(lines_);
    for (std::size_t i = 0; i < lines_; ++i) {
        result.emplace_back();
    }
    return result;
}

void Spacer::invalidate() {}

} // namespace cch::tui
