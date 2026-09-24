#include "coding_agent/tui/StatusIndicator.hpp"

#include "coding_agent/tui/KeybindingHints.hpp"
#include <cch/support/Error.hpp>
#include <cch/tui/Style.hpp>
#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

StatusIndicator::StatusIndicator(Kind kind,
        const LiveTheme& theme,
        cch::tui::RenderRequestSink request_render,
        std::string message,
        cch::tui::TextStyleHook working_color)
    : kind_(kind) {
    // pi WorkingStatusIndicator: the embedded colorFn recolors both the
    // spinner and the message (the same hook object in pi); the shared cell
    // keeps one dynamic hook alive for both loader slots.
    const auto shared_color =
            working_color ? std::make_shared<cch::tui::TextStyleHook>(std::move(working_color)) : nullptr;
    loader_ = std::make_unique<cch::tui::Loader>(cch::tui::LoaderOptions{
            .request_render = std::move(request_render),
            .spinner_style = shared_color ? cch::tui::TextStyleHook{[shared_color](std::string text) {
                return (*shared_color)(std::move(text));
            }}
                                          : (kind == Kind::Retry ? theme.foreground_hook(ThemeToken::Warning)
                                                                 : theme.foreground_hook(ThemeToken::Accent)),
            .message_style = shared_color ? cch::tui::TextStyleHook{[shared_color](std::string text) {
                return (*shared_color)(std::move(text));
            }}
                                          : theme.foreground_hook(ThemeToken::Muted),
            .message = std::move(message),
    });
    loader_->start();
}

void StatusIndicator::set_message(std::string message) {
    loader_->set_message(std::move(message));
}

support::Expected<cch::tui::RenderResult> StatusIndicator::render(std::size_t width) {
    return loader_->render(width);
}

void StatusIndicator::invalidate() {
    loader_->invalidate();
}

support::Expected<std::string> StatusIndicator::render_in_border(std::size_t width) {
    // pi: `super.render(width + 2)[1]` — the padded row leaves room for the
    // leading pad before the truncate-to-width cut.
    if (width == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI status border requires a positive width"));
    }
    auto rendered = loader_->render(width + 2);
    if (!rendered) return std::unexpected(rendered.error());
    auto line = rendered->lines.size() > 1 ? std::move(rendered->lines[1]) : std::string{};
    if (!line.empty() && line.front() == ' ') line.erase(line.begin());
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    return cch::tui::truncate_text(line, width, "");
}

support::Expected<std::string> StatusIndicator::render_spinner_in_border(std::size_t width) {
    if (width == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI status border requires a positive width"));
    }
    auto frame = loader_->rendered_indicator();
    if (!frame) return std::unexpected(frame.error());
    return cch::tui::truncate_text(*frame, width, "");
}

std::string working_status_message(std::string message) {
    // pi WorkingStatusIndicator: the plain "Working..." message (the
    // interrupt hint appears in the chat status line, not the indicator).
    return message;
}

std::string retry_status_message(
    const cch::tui::KeybindingRegistry& keybindings,
    int attempt,
    int max_attempts,
    int seconds) {
    // pi keyText: the raw key text ("escape"), not the capitalized display
    // form.
    const auto cancel_key = keybindings.key_text("app.interrupt");
    return std::format(
        "Retrying ({}/{}) in {}s... ({} to cancel)",
        attempt,
        max_attempts,
        seconds,
        cancel_key.empty() ? "Unbound" : cancel_key);
}

std::string compaction_status_message(
    const cch::tui::KeybindingRegistry& keybindings,
    std::string_view reason) {
    // pi keyText: the raw key text ("escape"), not the capitalized display
    // form.
    const auto cancel_key = keybindings.key_text("app.interrupt");
    const auto hint = std::format(
        "({} to cancel)",
        cancel_key.empty() ? "Unbound" : cancel_key);
    if (reason == "manual") {
        return "Compacting context... " + hint;
    }
    return reason == "overflow"
        ? "Context overflow detected, Auto-compacting... " + hint
        : "Auto-compacting... " + hint;
}

namespace {

/// The editor border run (pi's `"─".repeat(n)`); each rule column is the
/// three-byte U+2500 sequence.
[[nodiscard]] std::string border_run(std::size_t columns) {
    std::string run;
    run.reserve(columns * 3);
    for (std::size_t index = 0; index < columns; ++index)
        run += "─";
    return run;
}

[[nodiscard]] std::string border_styled(cch::tui::TextStyleHook& border_style, std::string text) {
    return border_style(std::move(text));
}

} // namespace

support::Expected<std::optional<std::string>> embedded_status_top_border(StatusIndicator& indicator,
        cch::tui::TextStyleHook& border_style,
        std::size_t width,
        std::size_t hidden_line_count) {
    // pi custom-editor.ts renderTopBorder: width <= 0 keeps the default
    // border; so does an empty status row.
    if (width == 0) return std::nullopt;

    auto status = indicator.render_in_border(std::max<std::size_t>(1, width - 5));
    if (!status) return std::unexpected(status.error());
    auto status_width = cch::tui::visible_width(*status);
    if (status_width == 0) return std::nullopt;

    const auto overflow_label = hidden_line_count > 0 ? std::format(" ↑ {} more ", hidden_line_count) : std::string{};
    const auto overflow_label_width = cch::tui::visible_width(overflow_label);
    const auto overflow_start = (width - overflow_label_width) / 2;
    const auto can_fit_overflow =
            !overflow_label.empty() && overflow_label_width + 2 <= width && overflow_start >= 3 + status_width + 1 + 1;

    if (can_fit_overflow) {
        const auto left_block_width = 3 + status_width + 1;
        return border_styled(border_style, "── ") + *status +
               border_styled(border_style,
                       " " + border_run(overflow_start - left_block_width) + overflow_label +
                               border_run(width - overflow_start - overflow_label_width));
    }

    if (width >= status_width + 5) {
        return border_styled(border_style, "── ") + *status +
               border_styled(border_style, " " + border_run(width - status_width - 4));
    }

    // Narrow: the spinner alone between border runs.
    auto spinner = indicator.render_spinner_in_border(width);
    if (!spinner) return std::unexpected(spinner.error());
    const auto spinner_width = cch::tui::visible_width(*spinner);
    const auto prefix_width = std::min<std::size_t>(3, width > spinner_width ? width - spinner_width : 0);
    return border_styled(border_style, border_run(prefix_width)) + *spinner +
           border_styled(border_style,
                   border_run(width > prefix_width + spinner_width ? width - prefix_width - spinner_width : 0));
}

} // namespace cch::coding_agent::tui
