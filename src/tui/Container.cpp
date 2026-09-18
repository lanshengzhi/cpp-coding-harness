#include <cch/tui/Container.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

Container::Container(Container&&) noexcept = default;
Container& Container::operator=(Container&&) noexcept = default;
Container::~Container() = default;

support::Expected<std::reference_wrapper<Component>> Container::add_child(
    std::unique_ptr<Component> component) {
    return detail::attach_child(children_, std::move(component), "Container");
}

void Container::clear() {
    children_.clear();
}

support::Expected<RenderResult> Container::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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
            result.lines.push_back(std::move(prepared->text));
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

void Box::clear_cache() {
    cached_result_ = {};
    cache_valid_ = false;
}

void Box::mark_children_changed() {
    ++children_revision_;
    clear_cache();
}

support::Expected<std::reference_wrapper<Component>> Box::add_child(
    std::unique_ptr<Component> component) {
    auto attached = detail::attach_child(children_, std::move(component), "Box");
    if (attached) mark_children_changed();
    return attached;
}

void Box::clear() {
    children_.clear();
    mark_children_changed();
}

void Box::set_background_hook(BackgroundHook background_hook) {
    background_hook_ = std::move(background_hook);
    ++background_hook_revision_;
    clear_cache();
}

support::Expected<RenderResult> Box::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI Box requires a positive visible width"));
    }
    if (padding_x_ >= width || padding_x_ >= width - padding_x_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI Box width is too small for padding",
            std::format("width {} padding_x {}", width, padding_x_)));
    }
    if (children_.empty()) {
        last_render_tokenize_calls_ = 0;
        return RenderResult{};
    }
    if (cache_valid_ && cached_width_ == width && cached_children_revision_ == children_revision_ &&
            cached_background_hook_revision_ == background_hook_revision_) {
        last_render_tokenize_calls_ = 0;
        return cached_result_;
    }

    last_render_tokenize_calls_ = 0;
    const auto content_width = width - padding_x_ - padding_x_;
    RenderResult result;
    const auto make_line = [&](std::string line, std::size_t line_width) -> support::Expected<std::string> {
        if (line_width > width) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "TUI Box composed a line wider than its width bound"));
        }
        line.append(width - line_width, ' ');
        return detail::apply_background(
                background_hook_, detail::PreparedRenderedLine{.text = std::move(line), .width = width}, width, "Box");
    };

    for (std::size_t index = 0; index < padding_y_; ++index) {
        auto line = make_line(std::string(width, ' '), width);
        if (!line) return std::unexpected(line.error());
        result.lines.push_back(std::move(*line));
    }

    for (const auto& child : children_) {
        auto rendered = child->render(content_width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = result.lines.size();
        for (auto& line : rendered->lines) {
            ++last_render_tokenize_calls_;
            auto prepared_child = detail::prepare_rendered_line(line, content_width);
            if (!prepared_child) return std::unexpected(prepared_child.error());
            auto prepared =
                    make_line(std::string(padding_x_, ' ') + prepared_child->text, padding_x_ + prepared_child->width);
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
        auto line = make_line(std::string(width, ' '), width);
        if (!line) return std::unexpected(line.error());
        result.lines.push_back(std::move(*line));
    }
    cached_result_ = result;
    cached_width_ = width;
    cached_children_revision_ = children_revision_;
    cached_background_hook_revision_ = background_hook_revision_;
    cache_valid_ = true;
    return result;
}

void Box::invalidate() {
    mark_children_changed();
    for (const auto& child : children_) child->invalidate();
}

Spacer::Spacer(std::size_t lines)
    : lines_(lines) {}

void Spacer::set_lines(std::size_t lines) {
    lines_ = lines;
}

support::Expected<RenderResult> Spacer::render(std::size_t) {
    return RenderResult{.lines = std::vector<std::string>(lines_)};
}

void Spacer::invalidate() {}

namespace detail::testing {

std::size_t box_tokenize_terminal_output_call_count(const Box& box) noexcept { return box.last_render_tokenize_calls_; }

} // namespace detail::testing

} // namespace cch::tui
