#pragma once

#include <cch/tui/Component.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// A Unicode-aware text component with padding and optional background.
///
/// Renders multi-line text with word wrapping. ANSI escape sequences
/// are preserved and do not count toward visible width.
class Text final : public Component {
public:
    explicit Text(
        std::string text = {},
        std::size_t padding_x = 1,
        std::size_t padding_y = 1,
        BackgroundHook background_hook = {});
    Text(Text&&) noexcept;
    Text& operator=(Text&&) noexcept;
    ~Text() override;

    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    void set_text(std::string text);
    [[nodiscard]] std::string_view text() const;

    void set_padding_x(std::size_t padding_x);
    void set_padding_y(std::size_t padding_y);
    void set_background_hook(BackgroundHook background_hook);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    std::string text_;
    std::size_t padding_x_;
    std::size_t padding_y_;
    BackgroundHook background_hook_;
    std::string cached_text_;
    std::size_t cached_width_{0};
    std::vector<std::string> cached_lines_;
    bool cache_valid_{false};
};

} // namespace cch::tui
