#pragma once

#include <cch/tui/Component.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// Decorates text without changing its terminal-visible width.
using MarkdownStyleHook = std::move_only_function<std::string(std::string)>;
/// Renders an already-styled label for a destination, optionally using OSC 8.
using MarkdownLinkHook = std::move_only_function<std::string(std::string, std::string_view)>;
/// Returns styled logical lines, or std::nullopt when the language is unsupported.
using SyntaxHighlightHook = std::move_only_function<
    std::optional<std::vector<std::string>>(std::string_view, std::string_view)>;

/// Generic terminal styling for Markdown semantic roles.
struct MarkdownStyleConfig {
    MarkdownStyleHook text{};
    MarkdownStyleHook heading{};
    MarkdownStyleHook emphasis{};
    MarkdownStyleHook strong{};
    MarkdownStyleHook strikethrough{};
    MarkdownStyleHook inline_code{};
    MarkdownStyleHook code_block{};
    MarkdownStyleHook code_block_border{};
    MarkdownStyleHook list_marker{};
    MarkdownStyleHook quote{};
    MarkdownStyleHook quote_border{};
    MarkdownStyleHook horizontal_rule{};
    MarkdownStyleHook link_text{};
    MarkdownStyleHook link_url{};
    MarkdownLinkHook link{};
    std::string code_block_indent{"  "};
};

/// A tolerant, Unicode-aware Markdown component with injected styling.
class Markdown final : public Component {
public:
    explicit Markdown(
        std::string text = {},
        std::size_t padding_x = 0,
        std::size_t padding_y = 0,
        MarkdownStyleConfig style = {},
        SyntaxHighlightHook syntax_highlighter = {},
        BackgroundHook background_hook = {});
    Markdown(Markdown&&) noexcept;
    Markdown& operator=(Markdown&&) noexcept;
    ~Markdown() override;

    Markdown(const Markdown&) = delete;
    Markdown& operator=(const Markdown&) = delete;

    void set_text(std::string text);
    [[nodiscard]] std::string_view text() const;

    void set_padding_x(std::size_t padding_x);
    void set_padding_y(std::size_t padding_y);
    void set_style(MarkdownStyleConfig style);
    void set_syntax_highlighter(SyntaxHighlightHook syntax_highlighter);
    void set_background_hook(BackgroundHook background_hook);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
