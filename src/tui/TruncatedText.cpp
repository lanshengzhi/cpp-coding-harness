#include <cch/tui/TruncatedText.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/util/Error.hpp>
#include <format>
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

util::Expected<RenderResult> TruncatedText::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI TruncatedText requires a positive visible width"));
    }
    if (padding_x_ >= width || padding_x_ >= width - padding_x_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI TruncatedText width is too small for padding",
            std::format("width {} padding_x {}", width, padding_x_)));
    }

    auto normalized = detail::normalize_terminal_output(text_);
    if (!normalized) return std::unexpected(normalized.error());
    const auto newline_position = normalized->find('\n');
    const auto single_line = std::string_view(*normalized).substr(0, newline_position);
    const auto available_width = width - padding_x_ - padding_x_;
    // Hard cut at the width boundary: no ellipsis (pi `TruncatedText`).
    auto truncated = truncate_text(single_line, available_width, "");
    if (!truncated) return std::unexpected(truncated.error());

    std::vector<std::string> result;
    for (std::size_t index = 0; index < padding_y_; ++index) {
        result.emplace_back(width, ' ');
    }

    std::string padded(padding_x_, ' ');
    padded += *truncated;
    const auto visible = visible_width(padded);
    if (visible < width) padded.append(width - visible, ' ');
    auto prepared = detail::prepare_rendered_line(padded, width);
    if (!prepared) return std::unexpected(prepared.error());
    result.push_back(std::move(*prepared));

    for (std::size_t index = 0; index < padding_y_; ++index) {
        result.emplace_back(width, ' ');
    }
    return RenderResult{.lines = std::move(result)};
}

void TruncatedText::invalidate() {}

} // namespace cch::tui
