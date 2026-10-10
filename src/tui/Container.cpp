#include <cch/tui/Container.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
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

std::unique_ptr<Component> Container::remove_child(const Component* component) {
    const auto found = std::find_if(
            children_.begin(), children_.end(), [component](const auto& child) { return child.get() == component; });
    if (found == children_.end()) return nullptr;

    auto removed = std::move(*found);
    children_.erase(found);
    return removed;
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
    cached_child_lines_.clear();
    cached_background_sample_.reset();
    cache_valid_ = false;
}

support::Expected<std::reference_wrapper<Component>> Box::add_child(
    std::unique_ptr<Component> component) {
    auto attached = detail::attach_child(children_, std::move(component), "Box");
    if (attached) clear_cache();
    return attached;
}

std::unique_ptr<Component> Box::remove_child(const Component* component) {
    const auto found = std::find_if(
            children_.begin(), children_.end(), [component](const auto& child) { return child.get() == component; });
    if (found == children_.end()) return nullptr;

    auto removed = std::move(*found);
    children_.erase(found);
    clear_cache();
    return removed;
}

void Box::clear() {
    children_.clear();
    clear_cache();
}

void Box::set_background_hook(BackgroundHook background_hook) {
    background_hook_ = std::move(background_hook);
}

support::Expected<RenderResult> Box::render(std::size_t width) {
    last_render_tokenize_calls_ = 0;
    if (children_.empty()) return RenderResult{};

    const auto padding_fits = width > 0 && padding_x_ <= (width - 1) / 2;
    const auto content_width = padding_fits ? width - padding_x_ * 2 : std::size_t{1};
    std::vector<std::string> child_lines;
    std::vector<InlineImageRenderRegion> images;
    for (const auto& child : children_) {
        auto rendered = child->render(content_width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = padding_y_ + child_lines.size();
        for (auto& line : rendered->lines)
            child_lines.push_back(std::move(line));
        for (auto& image : rendered->images) {
            image.region.column += padding_x_;
            image.region.row += row_offset;
            image.max_width = std::min(image.max_width.value_or(content_width), content_width);
            images.push_back(std::move(image));
        }
    }
    if (child_lines.empty() && images.empty()) return RenderResult{};

    const auto background_sample =
            background_hook_ ? std::optional<std::string>{background_hook_("test")} : std::nullopt;
    if (cache_valid_ && cached_width_ == width && cached_background_sample_ == background_sample &&
            cached_child_lines_ == child_lines) {
        auto result = cached_result_;
        result.images = std::move(images);
        return result;
    }

    const auto make_line = [&](std::string line) {
        const auto line_width = detail::measure_visible_width(line).width;
        if (line_width < width) line.append(width - line_width, ' ');
        if (background_hook_) line = background_hook_(std::move(line));
        return line;
    };

    RenderResult result;
    for (std::size_t index = 0; index < padding_y_; ++index)
        result.lines.push_back(make_line(std::string(width, ' ')));

    for (const auto& line : child_lines) {
        ++last_render_tokenize_calls_;
        auto normalized_child = detail::normalize_terminal_output(line);
        if (!normalized_child) return std::unexpected(normalized_child.error());
        result.lines.push_back(make_line(std::string(padding_x_, ' ') + std::move(*normalized_child)));
    }

    for (std::size_t index = 0; index < padding_y_; ++index)
        result.lines.push_back(make_line(std::string(width, ' ')));
    result.images = std::move(images);
    cached_result_ = result;
    cached_child_lines_ = std::move(child_lines);
    cached_background_sample_ = background_sample;
    cached_width_ = width;
    cache_valid_ = true;
    return result;
}

void Box::invalidate() {
    clear_cache();
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
