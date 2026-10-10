#include <cch/tui/TruncatedText.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {

TruncatedText::TruncatedText(
    std::string text,
    std::size_t padding_x,
    std::size_t padding_y)
    : text_(std::move(text)),
      padding_x_(padding_x),
      padding_y_(padding_y) {}

void TruncatedText::set_text(std::string text) {
    text_ = std::move(text);
}

std::string_view TruncatedText::text() const {
    return text_;
}

support::Expected<RenderResult> TruncatedText::render(std::size_t width) {
    auto normalized = detail::normalize_terminal_output(text_);
    if (!normalized) return std::unexpected(normalized.error());
    const auto newline_position = normalized->find('\n');
    const auto single_line = std::string_view(*normalized).substr(0, newline_position);
    const auto horizontal_padding = padding_x_ * 2;
    const auto available_width = width > horizontal_padding ? width - horizontal_padding : std::size_t{1};
    auto truncated = truncate_text(single_line, available_width);
    if (!truncated) return std::unexpected(truncated.error());

    std::vector<std::string> result;
    for (std::size_t index = 0; index < padding_y_; ++index) {
        result.emplace_back(width, ' ');
    }

    std::string padded(padding_x_, ' ');
    padded += *truncated;
    padded.append(padding_x_, ' ');
    const auto line_width = visible_width(padded);
    if (line_width < width) padded.append(width - line_width, ' ');
    result.push_back(std::move(padded));

    for (std::size_t index = 0; index < padding_y_; ++index) {
        result.emplace_back(width, ' ');
    }
    return RenderResult{.lines = std::move(result)};
}

void TruncatedText::invalidate() {}

} // namespace cch::tui
