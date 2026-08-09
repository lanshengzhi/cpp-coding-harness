#include "BashExecutionComponent.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

// Baseline preview bounds: pi 83114817 bash-execution.ts PREVIEW_LINES.
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

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\x1b') {
            if (index + 1 < text.size() && text[index + 1] == '[') {
                index += 2;
                while (index < text.size() &&
                       !(text[index] >= 0x40 && text[index] <= 0x7E)) {
                    ++index;
                }
                continue;
            }
            if (index + 1 < text.size() && text[index + 1] == ']') {
                index += 2;
                while (index < text.size() && text[index] != '\x07' &&
                       !(text[index] == '\x1b' && index + 1 < text.size() &&
                         text[index + 1] == '\\')) {
                    ++index;
                }
                continue;
            }
            continue;
        }
        result.push_back(text[index]);
    }
    return result;
}

[[nodiscard]] std::string horizontal_border(std::size_t width) {
    std::string border;
    border.reserve(width * 3);
    for (std::size_t index = 0; index < width; ++index) border += "─";
    return border;
}

void append_lines(std::vector<std::string>& destination, std::vector<std::string> lines) {
    destination.insert(
        destination.end(),
        std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
}

} // namespace

BashExecutionComponent::BashExecutionComponent(
    const LiveTheme& theme,
    std::shared_ptr<const SharedKeybindings> keybindings,
    std::string command,
    bool exclude_from_context)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      command_(std::move(command)),
      exclude_from_context_(exclude_from_context) {}

BashExecutionComponent::~BashExecutionComponent() = default;

void BashExecutionComponent::append_output(std::string chunk) {
    // pi bash-execution.ts appendOutput: strip ANSI and normalize endings.
    auto clean = strip_ansi(chunk);
    while (clean.find("\r\n") != std::string::npos) {
        clean.replace(clean.find("\r\n"), 2, "\n");
    }
    while (clean.find('\r') != std::string::npos) {
        clean.replace(clean.find('\r'), 1, "\n");
    }
    output_ += clean;
}

void BashExecutionComponent::set_complete(
    std::optional<int> exit_code,
    bool cancelled,
    bool truncated,
    std::optional<std::string> full_output_path) {
    exit_code_ = exit_code;
    cancelled_ = cancelled;
    truncated_ = truncated;
    full_output_path_ = std::move(full_output_path);
    running_ = false;
    if (loader_) loader_->stop();
}

void BashExecutionComponent::set_expanded(bool expanded) {
    expanded_ = expanded;
}

void BashExecutionComponent::start_loader(cch::tui::RenderRequestSink request_render) {
    if (!running_) return;
    if (loader_ && loader_->running()) return;
    const auto cancel_key = keybindings_->registry().key_text("app.interrupt");
    cch::tui::LoaderOptions options;
    options.request_render = std::move(request_render);
    options.spinner_style = theme_.foreground_hook(
        exclude_from_context_ ? ThemeToken::Dim : ThemeToken::BashMode);
    options.message_style = theme_.foreground_hook(ThemeToken::Muted);
    options.message = std::format(
        "Running... ({} to cancel)",
        cancel_key.empty() ? "Unbound" : cancel_key);
    loader_ = std::make_unique<cch::tui::Loader>(std::move(options));
    loader_->start();
}

util::Expected<cch::tui::RenderResult> BashExecutionComponent::render(std::size_t width) {
    const auto inclusion_token =
        exclude_from_context_ ? ThemeToken::Dim : ThemeToken::BashMode;
    std::vector<std::string> lines;

    // pi bash-execution.ts: spacer, top border, content, bottom border.
    lines.emplace_back();
    lines.push_back(theme_.foreground(inclusion_token, horizontal_border(width)));

    auto header = cch::tui::Text(
        theme_.foreground(
            inclusion_token,
            std::format("\x1b[1m$ {}\x1b[22m", bounded_presentation(command_))),
        1,
        0);
    auto rendered_header = header.render(width);
    if (!rendered_header) return std::unexpected(rendered_header.error());
    append_lines(lines, std::move(rendered_header->lines));

    const auto output = bounded_presentation(output_);
    const auto logical = output.empty()
        ? std::vector<std::string>{}
        : split_logical_lines(output);
    const auto preview_count = std::min(logical.size(), kPreviewLogicalLines);
    const auto hidden = logical.size() - preview_count;
    if (!logical.empty()) {
        if (expanded_) {
            // pi bash-execution.ts: the leading blank line is part of the
            // preview text, so the 20-visual-line window includes it.
            auto body = cch::tui::Text(
                theme_.foreground(ThemeToken::Muted, std::format("\n{}", output)),
                1,
                0);
            auto rendered_body = body.render(width);
            if (!rendered_body) return std::unexpected(rendered_body.error());
            append_lines(lines, std::move(rendered_body->lines));
        } else {
            std::string preview;
            for (std::size_t index = hidden; index < logical.size(); ++index) {
                if (!preview.empty()) preview.push_back('\n');
                preview += logical[index];
            }
            // pi bash-execution.ts truncateToVisualLines(`\n${styledOutput}`,
            // 20, width, 1): the leading blank rides inside the window.
            cch::tui::Text component(std::format("\n{}", preview), 1, 0);
            auto wrapped = component.render(width);
            if (!wrapped) return std::unexpected(wrapped.error());
            auto visual = std::move(wrapped->lines);
            if (visual.size() > kPreviewVisualLines) {
                visual.erase(
                    visual.begin(),
                    visual.end() - static_cast<std::ptrdiff_t>(kPreviewVisualLines));
            }
            for (auto& line : visual) {
                line = theme_.foreground(ThemeToken::Muted, std::move(line));
            }
            append_lines(lines, std::move(visual));
        }
    }

    if (running_ && loader_) {
        auto rendered_loader = loader_->render(width);
        if (!rendered_loader) return std::unexpected(rendered_loader.error());
        append_lines(lines, std::move(rendered_loader->lines));
    } else if (!running_) {
        const auto expand_key = keybindings_->registry().key_text("app.tools.expand");
        const auto hint = expand_key.empty() ? "Unbound" : expand_key;
        std::string status;
        if (hidden > 0) {
            status += theme_.foreground(
                ThemeToken::Muted,
                expanded_ ? "(" : std::format("... {} more lines (", hidden));
            status += theme_.foreground(ThemeToken::Dim, hint);
            status += theme_.foreground(
                ThemeToken::Muted,
                expanded_ ? " to collapse)" : " to expand)");
        }
        if (cancelled_) {
            if (!status.empty()) status.push_back('\n');
            status += theme_.foreground(ThemeToken::Warning, "(cancelled)");
        } else if (exit_code_ && *exit_code_ != 0) {
            if (!status.empty()) status.push_back('\n');
            status += theme_.foreground(
                ThemeToken::Error,
                std::format("(exit {})", *exit_code_));
        }
        if (truncated_ && full_output_path_) {
            if (!status.empty()) status.push_back('\n');
            status += theme_.foreground(
                ThemeToken::Warning,
                "Output truncated. Full output: " + *full_output_path_);
        }
        if (!status.empty()) {
            lines.emplace_back();
            cch::tui::Text component(std::move(status), 1, 0);
            auto rendered = component.render(width);
            if (!rendered) return std::unexpected(rendered.error());
            append_lines(lines, std::move(rendered->lines));
        }
    }

    lines.push_back(theme_.foreground(inclusion_token, horizontal_border(width)));
    return cch::tui::RenderResult{.lines = std::move(lines)};
}

void BashExecutionComponent::invalidate() {
    if (loader_) loader_->invalidate();
}

} // namespace cch::coding_agent::tui
