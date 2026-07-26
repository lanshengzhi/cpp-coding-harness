#include <cch/tui/Text.hpp>

#include "tui/TextValidation.hpp"

#include <algorithm>
#include <utility>

namespace cch::tui {

Text::Text(std::string text)
    : text_(std::move(text)) {}

void Text::set_text(std::string text) {
    text_ = std::move(text);
}

std::string_view Text::text() const {
    return text_;
}

util::Expected<std::vector<std::string>> Text::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI Text requires a positive visible width"));
    }
    if (auto result = detail::validate_ascii_text(text_); !result) {
        return std::unexpected(result.error());
    }

    std::vector<std::string> lines;
    std::size_t line_start = 0;
    while (line_start <= text_.size()) {
        const auto line_end = text_.find('\n', line_start);
        const auto source_end = line_end == std::string::npos ? text_.size() : line_end;
        const std::string_view source(text_.data() + line_start, source_end - line_start);

        if (source.empty()) {
            lines.emplace_back();
        } else {
            for (std::size_t offset = 0; offset < source.size(); offset += width) {
                lines.emplace_back(source.substr(offset, std::min(width, source.size() - offset)));
            }
        }

        if (line_end == std::string::npos) {
            break;
        }
        line_start = line_end + 1;
    }
    return lines;
}

void Text::invalidate() {}

} // namespace cch::tui
