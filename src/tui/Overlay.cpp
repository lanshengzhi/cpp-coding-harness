#include <cch/tui/Overlay.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {
} // namespace

struct Overlay::Impl {
    OverlayOptions options;
    bool visible_{true};
    std::vector<std::unique_ptr<Component>> children_;
    /// Anchor area (viewport-relative coordinates).
    std::size_t anchor_column{0};
    std::size_t anchor_row{0};
    std::size_t anchor_width{0};
    std::size_t anchor_height{0};
    bool focused_{false};
    Focusable* focused_child_{nullptr};

    void clamp_focus() {
        if (focused_child_ != nullptr) {
            bool found = false;
            for (const auto& child : children_) {
                if (child.get() == dynamic_cast<Component*>(focused_child_)) {
                    found = true;
                    break;
                }
            }
            if (!found) focused_child_ = nullptr;
        }
    }

    Focusable* first_focusable() const {
        for (const auto& child : children_) {
            auto* focusable = dynamic_cast<Focusable*>(child.get());
            if (focusable != nullptr) return focusable;
        }
        return nullptr;
    }

    };

Overlay::Overlay(OverlayOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
}

Overlay::Overlay(Overlay&&) noexcept = default;
Overlay& Overlay::operator=(Overlay&&) noexcept = default;
Overlay::~Overlay() = default;

support::Expected<std::reference_wrapper<Component>> Overlay::add_child(
    std::unique_ptr<Component> component) {
    return detail::attach_child(impl_->children_, std::move(component), "Overlay");
}

void Overlay::set_options(OverlayOptions options) {
    impl_->options = std::move(options);
    impl_->clamp_focus();
}

const OverlayOptions& Overlay::options() const {
    return impl_->options;
}

void Overlay::set_visible(bool visible) {
    impl_->visible_ = visible;
    if (!visible) {
        impl_->focused_ = false;
        if (impl_->focused_child_ != nullptr) impl_->focused_child_->set_focused(false);
    }
}

bool Overlay::visible() const {
    return impl_->visible_;
}

void Overlay::set_anchor(std::size_t column, std::size_t row, std::size_t width, std::size_t height) {
    impl_->anchor_column = column;
    impl_->anchor_row = row;
    impl_->anchor_width = width;
    impl_->anchor_height = height;
}

support::ExpectedVoid Overlay::focus_first() {
    auto* focusable = impl_->first_focusable();
    if (focusable == nullptr) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Overlay has no focusable children"));
    }
    if (impl_->focused_child_ != nullptr && impl_->focused_child_ != focusable) {
        impl_->focused_child_->set_focused(false);
    }
    impl_->focused_child_ = focusable;
    focusable->set_focused(true);
    impl_->focused_ = true;
    return {};
}

support::Expected<RenderResult> Overlay::render(std::size_t width) {
    if (!impl_->visible_) return RenderResult{};

    std::size_t effective_width = width;
    if (impl_->options.size_constraints.min_width) {
        effective_width = std::max(effective_width, *impl_->options.size_constraints.min_width);
    }
    if (impl_->options.size_constraints.max_width) {
        effective_width = std::min(effective_width, *impl_->options.size_constraints.max_width);
    }

    RenderResult result;
    for (const auto& child : impl_->children_) {
        auto rendered = child->render(effective_width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = result.lines.size();
        for (auto& line : rendered->lines) {
            auto prepared = detail::prepare_rendered_line(line, effective_width);
            if (!prepared) return std::unexpected(prepared.error());
            result.lines.push_back(std::move(*prepared));
        }
        for (auto& image : rendered->images) {
            image.region.row += row_offset;
            result.images.push_back(std::move(image));
        }
    }

    if (impl_->options.size_constraints.min_height) {
        while (result.lines.size() < *impl_->options.size_constraints.min_height) {
            result.lines.emplace_back(effective_width, ' ');
        }
    }
    if (impl_->options.size_constraints.max_height &&
        result.lines.size() > *impl_->options.size_constraints.max_height) {
        result.lines.resize(*impl_->options.size_constraints.max_height);
        std::erase_if(result.images, [&](const auto& image) {
            return image.region.row >= result.lines.size();
        });
    }

    return result;
}

void Overlay::invalidate() {
    for (const auto& child : impl_->children_) child->invalidate();
}

void Overlay::handle_input(const InputEventVariant& input) {
    // Forward to focused child that handles input
    // Non-capturing overlays skip input handling (handled by Tui dispatch)
    if (impl_->options.non_capturing) return;

    if (impl_->focused_child_ != nullptr) {
        auto* input_handler = dynamic_cast<InputHandler*>(impl_->focused_child_);
        if (input_handler != nullptr) {
            if (const auto* key = std::get_if<KeyEvent>(&input);
                key != nullptr && key->type == KeyEventType::Release && !input_handler->accepts_key_releases()) {
                return;
            }
            input_handler->handle_input(input);
        }
    }
}

bool Overlay::accepts_key_releases() const {
    if (impl_->focused_child_ != nullptr) {
        auto* input_handler = dynamic_cast<InputHandler*>(impl_->focused_child_);
        if (input_handler != nullptr) return input_handler->accepts_key_releases();
    }
    return false;
}

void Overlay::set_focused(bool focused) {
    impl_->focused_ = focused;
    if (focused && impl_->focused_child_ == nullptr) {
        impl_->focused_child_ = impl_->first_focusable();
    }
    if (impl_->focused_child_ != nullptr) impl_->focused_child_->set_focused(focused);
}

bool Overlay::focused() const {
    return impl_->focused_;
}

std::optional<CursorPosition> Overlay::cursor_location() const {
    if (!impl_->focused_ || impl_->focused_child_ == nullptr) return std::nullopt;
    return impl_->focused_child_->cursor_location();
}

std::pair<std::size_t, std::size_t> Overlay::layout_position(
    std::size_t viewport_width,
    std::size_t viewport_height,
    std::size_t content_width,
    std::size_t content_height) const {
    const auto& opts = impl_->options;
    switch (opts.position) {
    case OverlayPosition::TopLeft:
        return {impl_->anchor_column, impl_->anchor_row};
    case OverlayPosition::TopRight: {
        const auto col = impl_->anchor_column + impl_->anchor_width - std::min(content_width, impl_->anchor_width);
        return {col, impl_->anchor_row};
    }
    case OverlayPosition::BottomLeft: {
        const auto row = impl_->anchor_row + impl_->anchor_height - std::min(content_height, impl_->anchor_height);
        return {impl_->anchor_column, row};
    }
    case OverlayPosition::BottomRight: {
        const auto col = impl_->anchor_column + impl_->anchor_width - std::min(content_width, impl_->anchor_width);
        const auto row = impl_->anchor_row + impl_->anchor_height - std::min(content_height, impl_->anchor_height);
        return {col, row};
    }
    case OverlayPosition::Center: {
        const auto col = impl_->anchor_column + (impl_->anchor_width / 2) - (content_width / 2);
        const auto row = impl_->anchor_row + (impl_->anchor_height / 2) - (content_height / 2);
        return {col, row};
    }
    case OverlayPosition::TopCenter: {
        const auto col = impl_->anchor_column + (impl_->anchor_width / 2) - (content_width / 2);
        return {col, impl_->anchor_row};
    }
    case OverlayPosition::BottomCenter: {
        const auto col = impl_->anchor_column + (impl_->anchor_width / 2) - (content_width / 2);
        const auto row = impl_->anchor_row + impl_->anchor_height - std::min(content_height, impl_->anchor_height);
        return {col, row};
    }
    case OverlayPosition::LeftCenter: {
        const auto row = impl_->anchor_row + (impl_->anchor_height / 2) - (content_height / 2);
        return {impl_->anchor_column, row};
    }
    case OverlayPosition::RightCenter: {
        const auto col = impl_->anchor_column + impl_->anchor_width - std::min(content_width, impl_->anchor_width);
        const auto row = impl_->anchor_row + (impl_->anchor_height / 2) - (content_height / 2);
        return {col, row};
    }
    case OverlayPosition::Absolute:
        return {opts.absolute_column, opts.absolute_row};
    case OverlayPosition::Percentage: {
        const auto col = (viewport_width * opts.percentage_column) / 100;
        const auto row = (viewport_height * opts.percentage_row) / 100;
        return {col, row};
    }
    }
    return {impl_->anchor_column, impl_->anchor_row};
}

bool Overlay::visible_at(TerminalDimensions viewport) const {
    if (!impl_->visible_) return false;
    const auto& vis = impl_->options.visibility;
    if (vis.min_viewport_width && viewport.columns < *vis.min_viewport_width) return false;
    if (vis.min_viewport_height && viewport.rows < *vis.min_viewport_height) return false;
    if (vis.max_viewport_width && viewport.columns > *vis.max_viewport_width) return false;
    if (vis.max_viewport_height && viewport.rows > *vis.max_viewport_height) return false;
    return true;
}

} // namespace cch::tui
