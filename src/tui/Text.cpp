#include <cch/tui/Text.hpp>

#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

Text::Text(std::string text, std::size_t padding_x, std::size_t padding_y,
           std::function<std::string(std::string)> bg_fn)
    : text_(std::move(text))
    , padding_x_(padding_x)
    , padding_y_(padding_y)
    , bg_fn_(std::move(bg_fn)) {}

void Text::set_text(std::string text) {
    text_ = std::move(text);
    cache_valid_ = false;
}

std::string_view Text::text() const {
    return text_;
}

void Text::set_padding_x(std::size_t padding_x) {
    padding_x_ = padding_x;
    cache_valid_ = false;
}

void Text::set_padding_y(std::size_t padding_y) {
    padding_y_ = padding_y;
    cache_valid_ = false;
}

void Text::set_bg_fn(std::function<std::string(std::string)> bg_fn) {
    bg_fn_ = std::move(bg_fn);
    cache_valid_ = false;
}

util::Expected<std::vector<std::string>> Text::render(std::size_t width) {
    // Check cache
    if (cache_valid_ && cached_text_ == text_ && cached_width_ == width) {
        return cached_lines_;
    }

    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Text requires a positive visible width"));
    }

    // Empty text produces empty output
    if (text_.empty()) {
        cache_valid_ = true;
        cached_text_ = text_;
        cached_width_ = width;
        cached_lines_.clear();
        return cached_lines_;
    }

    // Calculate content width (subtract horizontal padding)
    const int content_width = static_cast<int>(width) - static_cast<int>(padding_x_ * 2);
    if (content_width <= 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Text width is too small for padding",
            "width " + std::to_string(width) +
                " padding_x " + std::to_string(padding_x_)));
    }

    // Normalize and wrap text
    auto wrapped = detail::wrap_text(text_, content_width);
    if (!wrapped) {
        return std::unexpected(wrapped.error());
    }

    // Build the result lines with padding and background
    std::vector<std::string> result;

    // Empty line template for top/bottom padding
    const std::string padding_line(width, ' ');

    // Empty line (no content, still padded)
    auto make_empty_line = [&](std::size_t w) -> std::string {
        auto line = padding_line.substr(0, w);
        if (bg_fn_) {
            return bg_fn_(line);
        }
        return line;
    };

    // Top padding
    const std::string empty_padded = make_empty_line(width);
    for (std::size_t i = 0; i < padding_y_; ++i) {
        result.push_back(empty_padded);
    }

    // Content lines
    const std::string left_margin(padding_x_, ' ');
    const std::string right_margin(padding_x_, ' ');

    for (const auto& line : *wrapped) {
        // Add left margin
        std::string padded = left_margin + line;
        // Add right margin
        auto line_vis = detail::visible_width(padded);
        if (static_cast<int>(width) > line_vis) {
            padded += std::string(width - line_vis, ' ');
        }

        if (bg_fn_) {
            padded = bg_fn_(padded);
        }

        result.push_back(std::move(padded));
    }

    // Bottom padding
    for (std::size_t i = 0; i < padding_y_; ++i) {
        result.push_back(empty_padded);
    }

    // Update cache
    cached_text_ = text_;
    cached_width_ = width;
    cached_lines_ = result;
    cache_valid_ = true;

    return result;
}

void Text::invalidate() {
    cache_valid_ = false;
}

} // namespace cch::tui
