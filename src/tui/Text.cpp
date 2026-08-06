#include <cch/tui/Text.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

Text::Text(
    std::string text,
    std::size_t padding_x,
    std::size_t padding_y,
    BackgroundHook background_hook)
    : text_(std::move(text)),
      padding_x_(padding_x),
      padding_y_(padding_y),
      background_hook_(std::move(background_hook)) {}

Text::Text(Text&&) noexcept = default;
Text& Text::operator=(Text&&) noexcept = default;
Text::~Text() = default;

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

void Text::set_background_hook(BackgroundHook background_hook) {
    background_hook_ = std::move(background_hook);
    cache_valid_ = false;
}

util::Expected<RenderResult> Text::render(std::size_t width) {
    if (cache_valid_ && cached_text_ == text_ && cached_width_ == width) {
        return RenderResult{.lines = cached_lines_};
    }
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Text requires a positive visible width"));
    }
    if (text_.empty()) {
        cached_text_ = text_;
        cached_width_ = width;
        cached_lines_.clear();
        cache_valid_ = true;
        return RenderResult{.lines = cached_lines_};
    }
    if (padding_x_ >= width || padding_x_ >= width - padding_x_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Text width is too small for padding",
            std::format("width {} padding_x {}", width, padding_x_)));
    }
    const auto content_width = width - padding_x_ - padding_x_;
    auto wrapped = wrap_text(text_, content_width);
    if (!wrapped) return std::unexpected(wrapped.error());

    std::vector<std::string> result;
    const auto make_line = [&](std::string line) -> util::Expected<std::string> {
        const auto visible = visible_width(line);
        if (visible < width) line.append(width - visible, ' ');
        return detail::apply_background(background_hook_, std::move(line), width, "Text");
    };

    for (std::size_t index = 0; index < padding_y_; ++index) {
        auto line = make_line(std::string(width, ' '));
        if (!line) return std::unexpected(line.error());
        result.push_back(std::move(*line));
    }
    for (const auto& line : *wrapped) {
        auto prepared = make_line(std::string(padding_x_, ' ') + line);
        if (!prepared) return std::unexpected(prepared.error());
        result.push_back(std::move(*prepared));
    }
    for (std::size_t index = 0; index < padding_y_; ++index) {
        auto line = make_line(std::string(width, ' '));
        if (!line) return std::unexpected(line.error());
        result.push_back(std::move(*line));
    }

    cached_text_ = text_;
    cached_width_ = width;
    cached_lines_ = result;
    cache_valid_ = true;
    return RenderResult{.lines = std::move(result)};
}

void Text::invalidate() {
    cache_valid_ = false;
}

} // namespace cch::tui
