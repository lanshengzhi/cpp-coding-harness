#include "BashBlock.hpp"

#include <cch/tui/Text.hpp>
#include "coding_agent/BoundedText.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

// Baseline preview bounds: pi 864b35c bash-execution.ts PREVIEW_LINES.
constexpr std::size_t kPreviewLogicalLines = 20;
constexpr std::size_t kPreviewVisualLines = 20;

/// Splits like pi's String.split("\n"): empty segments are kept, so a
/// trailing newline yields a trailing empty line.
[[nodiscard]] std::vector<std::string> split_logical_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            return lines;
        }
        lines.emplace_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    lines.emplace_back();
    return lines;
}

[[nodiscard]] util::Expected<std::vector<std::string>> render_styled(
    const LiveTheme& theme,
    std::string text,
    ThemeToken token,
    std::size_t width) {
    cch::tui::Text component(theme.foreground(token, std::move(text)), 0, 0);
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return std::move(rendered->lines);
}

void append_lines(std::vector<std::string>& destination, std::vector<std::string> lines) {
    destination.insert(
        destination.end(),
        std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
}

} // namespace

util::Expected<std::vector<std::string>> render_bash_block(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings,
    const BashBlockView& view,
    bool expanded,
    std::size_t width) {
    const auto inclusion_token =
        view.exclude_from_context ? ThemeToken::Dim : ThemeToken::BashMode;
    std::vector<std::string> lines;
    if (auto header = render_styled(
            theme,
            "$ " + bounded_redacted_presentation(view.command),
            inclusion_token,
            width);
        !header) {
        return std::unexpected(header.error());
    } else {
        append_lines(lines, std::move(*header));
    }

    const auto output = bounded_redacted_presentation(view.output);
    const auto logical = output.empty()
        ? std::vector<std::string>{}
        : split_logical_lines(output);
    const auto preview_count = std::min(logical.size(), kPreviewLogicalLines);
    const auto hidden = logical.size() - preview_count;
    if (!logical.empty()) {
        lines.emplace_back();
        if (expanded) {
            if (auto body = render_styled(theme, output, ThemeToken::Muted, width); !body) {
                return std::unexpected(body.error());
            } else {
                append_lines(lines, std::move(*body));
            }
        } else {
            std::string preview;
            for (std::size_t index = hidden; index < logical.size(); ++index) {
                if (!preview.empty()) preview.push_back('\n');
                preview += logical[index];
            }
            cch::tui::Text component(std::move(preview), 0, 0);
            auto wrapped = component.render(width);
            if (!wrapped) return std::unexpected(wrapped.error());
            auto visual = std::move(wrapped->lines);
            if (visual.size() > kPreviewVisualLines) {
                visual.erase(
                    visual.begin(),
                    visual.end() - static_cast<std::ptrdiff_t>(kPreviewVisualLines));
            }
            for (auto& line : visual) {
                line = theme.foreground(ThemeToken::Muted, std::move(line));
            }
            append_lines(lines, std::move(visual));
        }
    }

    if (!view.running) {
        const auto expand_key = keybindings.key_text("app.tools.expand");
        const auto hint = expand_key.empty() ? "Unbound" : expand_key;
        std::string status;
        if (hidden > 0) {
            status += theme.foreground(
                ThemeToken::Muted,
                expanded ? "(" : std::format("... {} more lines (", hidden));
            status += theme.foreground(ThemeToken::Dim, hint);
            status += theme.foreground(
                ThemeToken::Muted,
                expanded ? " to collapse)" : " to expand)");
        }
        if (view.cancelled) {
            if (!status.empty()) status.push_back('\n');
            status += theme.foreground(ThemeToken::Warning, "(cancelled)");
        } else if (view.exit_code && *view.exit_code != 0) {
            if (!status.empty()) status.push_back('\n');
            status += theme.foreground(
                ThemeToken::Error,
                std::format("(exit {})", *view.exit_code));
        }
        if (view.truncated && view.full_output_path) {
            if (!status.empty()) status.push_back('\n');
            status += theme.foreground(
                ThemeToken::Warning,
                "Output truncated. Full output: " + *view.full_output_path);
        }
        if (!status.empty()) {
            lines.emplace_back();
            cch::tui::Text component(std::move(status), 0, 0);
            auto rendered = component.render(width);
            if (!rendered) return std::unexpected(rendered.error());
            append_lines(lines, std::move(rendered->lines));
        }
    }
    return lines;
}

} // namespace cch::coding_agent::tui
