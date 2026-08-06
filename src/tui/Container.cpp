#include <cch/tui/Container.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

Container::Container(Container&&) noexcept = default;
Container& Container::operator=(Container&&) noexcept = default;
Container::~Container() = default;

util::Expected<std::reference_wrapper<Component>> Container::add_child(
    std::unique_ptr<Component> component) {
    return detail::attach_child(children_, std::move(component), "Container");
}

util::Expected<RenderResult> Container::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Container requires a positive visible width"));
    }

    RenderResult result;
    for (const auto& child : children_) {
        auto rendered = child->render(width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = result.lines.size();
        for (auto& line : rendered->lines) {
            auto prepared = detail::prepare_rendered_line(line, width);
            if (!prepared) return std::unexpected(prepared.error());
            result.lines.push_back(std::move(*prepared));
        }
        for (auto& image : rendered->images) {
            image.region.row += row_offset;
            result.images.push_back(std::move(image));
        }
    }
    return result;
}

void Container::invalidate() {
    for (const auto& child : children_) child->invalidate();
}

Box::Box(
    std::size_t padding_x,
    std::size_t padding_y,
    BackgroundHook background_hook)
    : padding_x_(padding_x),
      padding_y_(padding_y),
      background_hook_(std::move(background_hook)) {}

Box::Box(Box&&) noexcept = default;
Box& Box::operator=(Box&&) noexcept = default;
Box::~Box() = default;

util::Expected<std::reference_wrapper<Component>> Box::add_child(
    std::unique_ptr<Component> component) {
    return detail::attach_child(children_, std::move(component), "Box");
}

void Box::clear() {
    children_.clear();
}

util::Expected<RenderResult> Box::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Box requires a positive visible width"));
    }
    if (padding_x_ >= width || padding_x_ >= width - padding_x_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Box width is too small for padding",
            std::format("width {} padding_x {}", width, padding_x_)));
    }
    if (children_.empty()) return RenderResult{};

    const auto content_width = width - padding_x_ - padding_x_;
    RenderResult result;
    const auto make_line = [&](std::string line) -> util::Expected<std::string> {
        const auto visible = visible_width(line);
        if (visible < width) line.append(width - visible, ' ');
        return detail::apply_background(background_hook_, std::move(line), width, "Box");
    };

    for (std::size_t index = 0; index < padding_y_; ++index) {
        auto line = make_line(std::string(width, ' '));
        if (!line) return std::unexpected(line.error());
        result.lines.push_back(std::move(*line));
    }

    for (const auto& child : children_) {
        auto rendered = child->render(content_width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = result.lines.size();
        for (auto& line : rendered->lines) {
            auto prepared_child = detail::prepare_rendered_line(line, content_width);
            if (!prepared_child) return std::unexpected(prepared_child.error());
            auto prepared = make_line(std::string(padding_x_, ' ') + *prepared_child);
            if (!prepared) return std::unexpected(prepared.error());
            result.lines.push_back(std::move(*prepared));
        }
        for (auto& image : rendered->images) {
            image.region.column += padding_x_;
            image.region.row += row_offset;
            image.max_width = std::min(
                image.max_width.value_or(content_width),
                content_width);
            result.images.push_back(std::move(image));
        }
    }

    for (std::size_t index = 0; index < padding_y_; ++index) {
        auto line = make_line(std::string(width, ' '));
        if (!line) return std::unexpected(line.error());
        result.lines.push_back(std::move(*line));
    }
    return result;
}

void Box::invalidate() {
    for (const auto& child : children_) child->invalidate();
}

Spacer::Spacer(std::size_t lines)
    : lines_(lines) {}

void Spacer::set_lines(std::size_t lines) {
    lines_ = lines;
}

util::Expected<RenderResult> Spacer::render(std::size_t) {
    return RenderResult{.lines = std::vector<std::string>(lines_)};
}

void Spacer::invalidate() {}

} // namespace cch::tui
