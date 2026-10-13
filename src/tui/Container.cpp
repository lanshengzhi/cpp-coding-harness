#include <cch/tui/Container.hpp>

#include "tui/MouseRouting.hpp"
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

support::Expected<std::reference_wrapper<Component>> Container::add_child(std::unique_ptr<Component> component) {
    auto attached = detail::attach_child(children_, std::move(component), "Container");
    if (attached) {
        rendered_child_rows_.clear();
        render_valid_ = false;
    }
    return attached;
}

std::unique_ptr<Component> Container::remove_child(const Component* component) {
    const auto found = std::find_if(
            children_.begin(), children_.end(), [component](const auto& child) { return child.get() == component; });
    if (found == children_.end()) return nullptr;

    auto removed = std::move(*found);
    children_.erase(found);
    rendered_child_rows_.clear();
    render_valid_ = false;
    return removed;
}

void Container::clear() {
    children_.clear();
    rendered_child_rows_.clear();
    render_valid_ = false;
}

support::Expected<RenderResult> Container::render(std::size_t width) {
    rendered_child_rows_.clear();
    render_valid_ = false;
    if (width == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI Container requires a positive visible width"));
    }

    RenderResult result;
    std::vector<std::size_t> rendered_child_rows;
    rendered_child_rows.reserve(children_.size());
    for (const auto& child : children_) {
        auto rendered = child->render(width);
        if (!rendered) {
            rendered_child_rows_.clear();
            render_valid_ = false;
            return std::unexpected(rendered.error());
        }
        const auto row_offset = result.lines.size();
        rendered_child_rows.push_back(rendered->lines.size());
        for (auto& line : rendered->lines) {
            auto prepared = detail::prepare_rendered_line(line, width);
            if (!prepared) {
                rendered_child_rows_.clear();
                render_valid_ = false;
                return std::unexpected(prepared.error());
            }
            result.lines.push_back(std::move(prepared->text));
        }
        for (auto& image : rendered->images) {
            image.region.row += row_offset;
            result.images.push_back(std::move(image));
        }
    }
    rendered_child_rows_ = std::move(rendered_child_rows);
    render_valid_ = true;
    return result;
}

void Container::invalidate() {
    rendered_child_rows_.clear();
    render_valid_ = false;
    for (const auto& child : children_)
        child->invalidate();
}

std::optional<CursorPosition> Container::cursor_location() const {
    if (!render_valid_ || rendered_child_rows_.size() != children_.size()) return std::nullopt;

    std::size_t row_offset = 0;
    for (std::size_t index = 0; index < children_.size(); ++index) {
        if (const auto loc = children_[index]->cursor_location()) {
            return CursorPosition{.column = loc->column, .row = row_offset + loc->row};
        }
        row_offset += rendered_child_rows_[index];
    }
    return std::nullopt;
}

bool Container::contains(const Component* target) const {
    if (this == target) return true;
    for (const auto& child : children_) {
        if (child && child->contains(target)) return true;
    }
    return false;
}

std::optional<MouseDispatchResult> Container::handle_mouse_forwarded(const MouseEvent& event) {
    // pi re-renders the children to measure them when the cached mouse layout
    // was built for another width (`tui.ts:375-376`). The frame is the geometry
    // authority here, so a hit test uses the last composition and never renders.
    if (!render_valid_ || rendered_child_rows_.size() != children_.size()) return std::nullopt;
    return detail::dispatch_mouse_to_cached_rows(
            event,
            children_.size(),
            [this](std::size_t index) -> Component& { return *children_[index]; },
            [this](std::size_t index) { return rendered_child_rows_[index]; });
}

Box::Box(std::size_t padding_x, std::size_t padding_y, BackgroundHook background_hook)
    : padding_x_(padding_x), padding_y_(padding_y), background_hook_(std::move(background_hook)) {}

Box::Box(Box&&) noexcept = default;
Box& Box::operator=(Box&&) noexcept = default;
Box::~Box() = default;

void Box::clear_cache() {
    cached_result_ = {};
    cached_child_lines_.clear();
    rendered_child_rows_.clear();
    cached_background_sample_.reset();
    rendered_padding_fits_ = false;
    render_valid_ = false;
    cache_valid_ = false;
}

support::Expected<std::reference_wrapper<Component>> Box::add_child(std::unique_ptr<Component> component) {
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

void Box::set_background_hook(BackgroundHook background_hook) { background_hook_ = std::move(background_hook); }

support::Expected<RenderResult> Box::render(std::size_t width) {
    last_render_tokenize_calls_ = 0;
    rendered_child_rows_.clear();
    rendered_padding_fits_ = false;
    render_valid_ = false;
    if (children_.empty()) {
        clear_cache();
        return RenderResult{};
    }

    const auto padding_fits = width > 0 && padding_x_ <= (width - 1) / 2;
    const auto horizontal_padding = padding_fits ? padding_x_ : std::size_t{0};
    const auto content_width = padding_fits ? width - padding_x_ * 2 : std::size_t{1};
    std::vector<std::string> child_lines;
    std::vector<std::size_t> rendered_child_rows;
    std::vector<InlineImageRenderRegion> images;
    rendered_child_rows.reserve(children_.size());
    for (const auto& child : children_) {
        auto rendered = child->render(content_width);
        if (!rendered) {
            clear_cache();
            return std::unexpected(rendered.error());
        }
        const auto row_offset = padding_y_ + child_lines.size();
        rendered_child_rows.push_back(rendered->lines.size());
        for (auto& line : rendered->lines)
            child_lines.push_back(std::move(line));
        for (auto& image : rendered->images) {
            image.region.column += horizontal_padding;
            image.region.row += row_offset;
            image.max_width = std::min(image.max_width.value_or(content_width), content_width);
            images.push_back(std::move(image));
        }
    }
    if (child_lines.empty() && images.empty()) {
        render_valid_ = false;
        return RenderResult{};
    }

    const auto background_sample =
            background_hook_ ? std::optional<std::string>{background_hook_("test")} : std::nullopt;
    if (cache_valid_ && cached_width_ == width && cached_background_sample_ == background_sample &&
            cached_child_lines_ == child_lines) {
        auto result = cached_result_;
        result.images = std::move(images);
        rendered_child_rows_ = std::move(rendered_child_rows);
        rendered_padding_fits_ = padding_fits;
        render_valid_ = true;
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
        if (!normalized_child) {
            clear_cache();
            return std::unexpected(normalized_child.error());
        }
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
    rendered_child_rows_ = std::move(rendered_child_rows);
    rendered_padding_fits_ = padding_fits;
    render_valid_ = true;
    return result;
}

void Box::invalidate() {
    clear_cache();
    for (const auto& child : children_)
        child->invalidate();
}

std::optional<CursorPosition> Box::cursor_location() const {
    if (!render_valid_ || rendered_child_rows_.size() != children_.size()) return std::nullopt;

    std::size_t row_offset = padding_y_;
    const auto column_offset = rendered_padding_fits_ ? padding_x_ : std::size_t{0};
    for (std::size_t index = 0; index < children_.size(); ++index) {
        if (const auto loc = children_[index]->cursor_location()) {
            return CursorPosition{.column = column_offset + loc->column, .row = row_offset + loc->row};
        }
        row_offset += rendered_child_rows_[index];
    }
    return std::nullopt;
}

bool Box::contains(const Component* target) const {
    if (this == target) return true;
    for (const auto& child : children_) {
        if (child && child->contains(target)) return true;
    }
    return false;
}

std::optional<MouseDispatchResult> Box::handle_mouse_forwarded(const MouseEvent& event) {
    if (!render_valid_ || rendered_child_rows_.size() != children_.size()) return std::nullopt;

    // The padded content area of the last composition: `render` falls back to a
    // single content column when the padding does not fit (pi
    // `components/box.ts:76-78`).
    const auto padding_x = rendered_padding_fits_ ? padding_x_ : std::size_t{0};
    const auto content_width =
            rendered_padding_fits_ && cached_width_ > padding_x * 2 ? cached_width_ - padding_x * 2 : std::size_t{1};
    const auto content_y = event.y - static_cast<std::ptrdiff_t>(padding_y_);
    const auto content_x = event.x - static_cast<std::ptrdiff_t>(padding_x);
    if (content_y < 0 || content_x < 0 || static_cast<std::size_t>(content_x) >= content_width) return std::nullopt;

    std::ptrdiff_t child_y = 0;
    for (std::size_t index = 0; index < children_.size(); ++index) {
        const auto child_height = rendered_child_rows_[index];
        if (content_y >= child_y && content_y < child_y + static_cast<std::ptrdiff_t>(child_height)) {
            auto child_event = event;
            child_event.x = content_x;
            child_event.y = content_y - child_y;
            child_event.width = content_width;
            child_event.height = child_height;
            return dispatch_mouse_event(*children_[index], child_event);
        }
        child_y += static_cast<std::ptrdiff_t>(child_height);
    }
    return std::nullopt;
}

Spacer::Spacer(std::size_t lines) : lines_(lines) {}

void Spacer::set_lines(std::size_t lines) { lines_ = lines; }

support::Expected<RenderResult> Spacer::render(std::size_t) {
    return RenderResult{.lines = std::vector<std::string>(lines_)};
}

void Spacer::invalidate() {}

namespace detail::testing {

std::size_t box_tokenize_terminal_output_call_count(const Box& box) noexcept { return box.last_render_tokenize_calls_; }

} // namespace detail::testing

} // namespace cch::tui
