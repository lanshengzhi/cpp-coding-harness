#include <cch/tui/TruncatedText.hpp>

#include "tui/UnicodeWidth.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

TruncatedText::TruncatedText(std::string text, std::string ellipsis,
                             std::size_t padding_x, std::size_t padding_y)
    : text_(std::move(text))
    , ellipsis_(std::move(ellipsis))
    , padding_x_(padding_x)
    , padding_y_(padding_y) {}

void TruncatedText::set_text(std::string text) {
    text_ = std::move(text);
}

std::string_view TruncatedText::text() const {
    return text_;
}

void TruncatedText::set_ellipsis(std::string ellipsis) {
    ellipsis_ = std::move(ellipsis);
}

util::Expected<std::vector<std::string>> TruncatedText::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI TruncatedText requires a positive visible width"));
    }

    std::vector<std::string> result;

    // Empty line template for padding
    const std::string empty_line(width, ' ');

    // Top padding
    for (std::size_t i = 0; i < padding_y_; ++i) {
        result.push_back(empty_line);
    }

    // Calculate available width for content after padding
    const auto available_width = static_cast<int>(width) - static_cast<int>(padding_x_ * 2);
    if (available_width <= 0) {
        // Width too small for padding — just render padding
        result.push_back(empty_line);
        return result;
    }

    // Take only the first line (stop at newline)
    std::string_view single_line = text_;
    const auto nl = text_.find('\n');
    if (nl != std::string_view::npos) {
        single_line = text_.substr(0, nl);
    }

    // Truncate
    auto truncated = detail::truncate_text(single_line, available_width, ellipsis_, false);
    if (!truncated) {
        return std::unexpected(truncated.error());
    }

    // Add horizontal padding
    std::string padded = std::string(padding_x_, ' ') + *truncated;

    // Pad to full width
    auto vis_width = detail::visible_width(padded);
    if (static_cast<int>(width) > vis_width) {
        padded += std::string(width - vis_width, ' ');
    }

    result.push_back(std::move(padded));

    // Bottom padding
    for (std::size_t i = 0; i < padding_y_; ++i) {
        result.push_back(empty_line);
    }

    return result;
}

void TruncatedText::invalidate() {}

} // namespace cch::tui
