#include <cch/tui/Text.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/RenderUtils.hpp"
#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

[[nodiscard]] bool is_trim_whitespace(std::string_view text) {
    std::size_t position = 0;
    while (position < text.size()) {
        const auto [codepoint, bytes] = detail::decode_utf8(text, position);
        if (bytes == 0) return false;
        const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
        const bool ecmascript_whitespace =
                (codepoint >= '\t' && codepoint <= '\r') || codepoint == ' ' || codepoint == 0x00A0 ||
                codepoint == 0x1680 || (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 ||
                codepoint == 0x2029 || codepoint == 0x202F || codepoint == 0x205F || codepoint == 0x3000 ||
                codepoint == 0xFEFF || category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL ||
                category == UTF8PROC_CATEGORY_ZP;
        if (!ecmascript_whitespace) return false;
        position += bytes;
    }
    return true;
}

} // namespace

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

support::Expected<RenderResult> Text::render(std::size_t width) {
    if (cache_valid_ && cached_text_ == text_ && cached_width_ == width) {
        return RenderResult{.lines = cached_lines_};
    }
    if (is_trim_whitespace(text_)) {
        cached_text_ = text_;
        cached_width_ = width;
        cached_lines_.clear();
        cache_valid_ = true;
        return RenderResult{.lines = cached_lines_};
    }
    std::string normalized_text;
    normalized_text.reserve(text_.size());
    for (const auto character : text_) {
        if (character == '\t')
            normalized_text += "   ";
        else
            normalized_text.push_back(character);
    }

    const auto effective_padding_x = width > 1 ? std::min(padding_x_, (width - 1) / 2) : 0;
    const auto content_width = std::max<std::size_t>(1, width - effective_padding_x - effective_padding_x);
    auto wrapped = wrap_text(normalized_text, content_width);
    if (!wrapped) return std::unexpected(wrapped.error());

    std::vector<std::string> result;
    const auto make_line = [&](std::string line, std::size_t line_width) {
        if (line_width < width) line.append(width - line_width, ' ');
        if (background_hook_) line = background_hook_(std::move(line));
        return line;
    };

    for (std::size_t index = 0; index < padding_y_; ++index) {
        result.push_back(make_line(std::string(width, ' '), width));
    }
    for (const auto& line : *wrapped) {
        auto normalized_line = detail::normalize_terminal_output(line);
        if (!normalized_line) return std::unexpected(normalized_line.error());
        const auto line_width = visible_width(*normalized_line);
        result.push_back(make_line(
                std::string(effective_padding_x, ' ') + std::move(*normalized_line), effective_padding_x + line_width));
    }
    for (std::size_t index = 0; index < padding_y_; ++index) {
        result.push_back(make_line(std::string(width, ' '), width));
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
