#pragma once

#include <cch/tui/Component.hpp>

#include <string>
#include <string_view>

namespace cch::tui {

/// A single-line text component that truncates to fit within the supplied width.
///
/// Renders at most one line of text. If the text is wider than the available
/// space, it is truncated with an ellipsis. Handles ANSI escape sequences
/// and Unicode display widths correctly.
class TruncatedText final : public Component {
public:
    /// @param text       Text content (only first line is shown)
    /// @param ellipsis   Ellipsis string for truncation (default "...")
    /// @param padding_x  Left/right padding (default 0)
    /// @param padding_y  Top/bottom padding (default 0)
    explicit TruncatedText(std::string text = {},
                           std::string ellipsis = "...",
                           std::size_t padding_x = 0,
                           std::size_t padding_y = 0);

    void set_text(std::string text);
    [[nodiscard]] std::string_view text() const;
    void set_ellipsis(std::string ellipsis);

    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) override;
    void invalidate() override;

private:
    std::string text_;
    std::string ellipsis_;
    std::size_t padding_x_;
    std::size_t padding_y_;
};

} // namespace cch::tui
