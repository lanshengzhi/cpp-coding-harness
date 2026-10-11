#include <cch/tui/Overlay.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

[[nodiscard]] std::optional<double> parse_size_value(std::string_view text) {
    if (!text.ends_with('%')) return std::nullopt;
    text.remove_suffix(1);
    if (text.empty()) return std::nullopt;

    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < 0.0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::size_t size_from_value(SizeValue value, std::size_t reference) {
    if (!value.valid || !std::isfinite(value.value) || value.value <= 0.0) return 0;
    const auto resolved = value.percentage
                                  ? static_cast<long double>(reference) * static_cast<long double>(value.value) / 100.0L
                                  : static_cast<long double>(value.value);
    if (resolved >= static_cast<long double>(std::numeric_limits<std::size_t>::max()))
        return std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(resolved);
}

[[nodiscard]] std::size_t available_after_margins(std::size_t dimension, std::size_t first, std::size_t second) {
    const auto after_first = dimension > first ? dimension - first : std::size_t{0};
    const auto after_both = after_first > second ? after_first - second : std::size_t{0};
    return std::max<std::size_t>(1, after_both);
}

[[nodiscard]] std::size_t add_offset(std::size_t position, std::ptrdiff_t offset) {
    if (offset >= 0) {
        const auto positive_offset = static_cast<std::size_t>(offset);
        if (position > std::numeric_limits<std::size_t>::max() - positive_offset)
            return std::numeric_limits<std::size_t>::max();
        return position + positive_offset;
    }
    const auto magnitude = static_cast<std::size_t>(-(offset + 1)) + 1;
    return position > magnitude ? position - magnitude : 0;
}

[[nodiscard]] std::size_t clamped_position(
        std::size_t position, std::ptrdiff_t offset, std::size_t lower, std::size_t upper) {
    const auto adjusted = add_offset(position, offset);
    return std::clamp(adjusted, lower, std::max(lower, upper));
}

[[nodiscard]] std::size_t anchor_column(
        OverlayAnchor anchor, std::size_t margin_left, std::size_t available_width, std::size_t width) {
    const auto max_column = available_width > width ? available_width - width : std::size_t{0};
    switch (anchor) {
    case OverlayAnchor::TopLeft:
    case OverlayAnchor::LeftCenter:
    case OverlayAnchor::BottomLeft:
        return margin_left;
    case OverlayAnchor::TopRight:
    case OverlayAnchor::RightCenter:
    case OverlayAnchor::BottomRight:
        return margin_left + max_column;
    case OverlayAnchor::Center:
    case OverlayAnchor::TopCenter:
    case OverlayAnchor::BottomCenter:
        return margin_left + max_column / 2;
    }
    return margin_left + max_column / 2;
}

[[nodiscard]] std::size_t anchor_row(
        OverlayAnchor anchor, std::size_t margin_top, std::size_t available_height, std::size_t height) {
    const auto max_row = available_height > height ? available_height - height : std::size_t{0};
    switch (anchor) {
    case OverlayAnchor::TopLeft:
    case OverlayAnchor::TopCenter:
    case OverlayAnchor::TopRight:
        return margin_top;
    case OverlayAnchor::BottomLeft:
    case OverlayAnchor::BottomCenter:
    case OverlayAnchor::BottomRight:
        return margin_top + max_row;
    case OverlayAnchor::Center:
    case OverlayAnchor::LeftCenter:
    case OverlayAnchor::RightCenter:
        return margin_top + max_row / 2;
    }
    return margin_top + max_row / 2;
}

[[nodiscard]] std::size_t percent_position(std::size_t available, double percent) {
    if (!std::isfinite(percent) || percent <= 0.0) return 0;
    const auto position = static_cast<long double>(available) * static_cast<long double>(percent) / 100.0L;
    if (position >= static_cast<long double>(available)) return available;
    return static_cast<std::size_t>(position);
}

} // namespace

SizeValue::SizeValue(const char* value_text) : SizeValue(std::string_view(value_text == nullptr ? "" : value_text)) {}

SizeValue::SizeValue(std::string_view value_text) {
    if (const auto parsed = parse_size_value(value_text)) {
        value = *parsed;
        percentage = true;
        return;
    }
    valid = false;
}

SizeValue::SizeValue(const std::string& value_text) : SizeValue(std::string_view(value_text)) {}

struct Overlay::Impl {
    OverlayOptions options;
    bool visible_{true};
    std::vector<std::unique_ptr<Component>> children_;
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

    RenderResult result;
    for (const auto& child : impl_->children_) {
        auto rendered = child->render(width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto row_offset = result.lines.size();
        if (impl_->focused_ && child.get() == dynamic_cast<Component*>(impl_->focused_child_)) {
            if (const auto cursor = impl_->focused_child_->cursor_location()) {
                result.cursor = CursorPosition{.column = cursor->column, .row = row_offset + cursor->row};
            }
        }
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

void Overlay::invalidate() {
    for (const auto& child : impl_->children_) child->invalidate();
}

InputAdmissionOutcome Overlay::handle_input(const InputEventVariant& input) {
    if (impl_->options.non_capturing) return InputAdmissionOutcome::Unhandled;

    const auto* key = std::get_if<KeyEvent>(&input);
    const bool is_release = key != nullptr && key->type == KeyEventType::Release;

    if (impl_->focused_child_ != nullptr) {
        auto* input_handler = dynamic_cast<InputHandler*>(impl_->focused_child_);
        if (input_handler != nullptr) {
            const auto outcome = input_handler->handle_input(input);
            if (is_release) return outcome;
            return InputAdmissionOutcome::Consumed;
        }
    }
    if (is_release) return InputAdmissionOutcome::Unhandled;
    return InputAdmissionOutcome::Consumed;
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

OverlayLayout Overlay::resolve_layout(TerminalDimensions viewport, std::size_t content_height) const {
    const auto& options = impl_->options;
    const auto& margins = options.margins;
    const auto available_width = available_after_margins(viewport.columns, margins.left, margins.right);
    const auto available_height = available_after_margins(viewport.rows, margins.top, margins.bottom);

    auto width = options.width.valid ? size_from_value(options.width, viewport.columns)
                                     : std::min<std::size_t>(80, available_width);
    width = std::max(width, options.min_width);
    width = std::clamp<std::size_t>(width, 1, available_width);

    std::optional<std::size_t> max_height;
    if (options.max_height && options.max_height->valid) {
        max_height = std::clamp<std::size_t>(size_from_value(*options.max_height, viewport.rows), 1, available_height);
    }
    const auto effective_height = max_height ? std::min(content_height, *max_height) : content_height;

    const auto position_col = [&] {
        if (!options.col) return anchor_column(options.anchor, margins.left, available_width, width);
        if (!options.col->valid) return anchor_column(options.anchor, margins.left, available_width, width);
        if (options.col->percentage) {
            const auto max_column = available_width > width ? available_width - width : std::size_t{0};
            return margins.left + percent_position(max_column, options.col->value);
        }
        return size_from_value(*options.col, std::numeric_limits<std::size_t>::max());
    }();

    const auto position_row = [&] {
        if (!options.row) return anchor_row(options.anchor, margins.top, available_height, effective_height);
        if (!options.row->valid) return anchor_row(options.anchor, margins.top, available_height, effective_height);
        if (options.row->percentage) {
            const auto max_row =
                    available_height > effective_height ? available_height - effective_height : std::size_t{0};
            return margins.top + percent_position(max_row, options.row->value);
        }
        return size_from_value(*options.row, std::numeric_limits<std::size_t>::max());
    }();

    const auto right_edge = viewport.columns > margins.right ? viewport.columns - margins.right : std::size_t{0};
    const auto bottom_edge = viewport.rows > margins.bottom ? viewport.rows - margins.bottom : std::size_t{0};
    const auto max_col = right_edge > width ? right_edge - width : std::size_t{0};
    const auto max_row = bottom_edge > effective_height ? bottom_edge - effective_height : std::size_t{0};

    return OverlayLayout{
            .width = width,
            .row = clamped_position(position_row, options.offset_y, margins.top, max_row),
            .col = clamped_position(position_col, options.offset_x, margins.left, max_col),
            .max_height = max_height,
            .effective_height = effective_height,
    };
}

bool Overlay::visible_at(TerminalDimensions viewport) const {
    if (!impl_->visible_) return false;
    return !impl_->options.visible_at || impl_->options.visible_at(viewport);
}

} // namespace cch::tui
