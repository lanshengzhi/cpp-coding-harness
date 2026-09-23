#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/tui/Style.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent::tui {

/// pi `status-indicator.ts`: the status container's one active indicator
/// (Working/Retry/Compaction) rendered through the TUI Loader — one empty
/// spacer row plus the spinner-and-message row (pi's Loader renders
/// `["", ...]`). The active indicator owns the loader and replaces the
/// previous one; `IdleStatus` renders two empty rows.
class StatusIndicator final : public cch::tui::Component {
public:
    enum class Kind { Working, Retry, Compaction };

    /// Constructs the indicator; `request_render` fires on every animation
    /// frame and countdown tick (pi's Loader `requestRender`). `working_color`
    /// (pi's embedded `WorkingStatusIndicator` colorFn) recolors both the
    /// spinner and the message with the thinking-level border color when the
    /// indicator embeds into the editor border.
    StatusIndicator(
        Kind kind,
        const LiveTheme& theme,
        cch::tui::RenderRequestSink request_render,
        std::string message,
        cch::tui::TextStyleHook working_color = {});

    [[nodiscard]] Kind kind() const { return kind_; }

    /// pi `StatusIndicator.setMessage` (the retry countdown rewrites the
    /// message on every tick).
    void set_message(std::string message);

    /// pi `StatusIndicator.renderInBorder`: the loader's message row without
    /// the leading pad, trailing spaces trimmed, truncated to `width` with no
    /// ellipsis (pi `truncateToWidth(_, width, "")).
    [[nodiscard]] support::Expected<std::string> render_in_border(std::size_t width);

    /// pi `StatusIndicator.renderSpinnerInBorder`: the styled spinner frame
    /// alone, truncated to `width` with no ellipsis.
    [[nodiscard]] support::Expected<std::string> render_spinner_in_border(std::size_t width);

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    Kind kind_;
    std::unique_ptr<cch::tui::Loader> loader_;
};

/// pi `IdleStatus`: the status container's empty state (two blank rows).
class IdleStatus final : public cch::tui::Component {
public:
    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        return cch::tui::RenderResult{
            .lines = {std::string(width, ' '), std::string(width, ' ')},
        };
    }
    void invalidate() override {}
};

/// pi `WorkingStatusIndicator`: accent spinner, muted message.
[[nodiscard]] std::string working_status_message(std::string message);

/// pi `RetryStatusIndicator` message: `Retrying (attempt/max) in Ns... (Esc
/// to cancel)` — the countdown text uses the assembled `app.interrupt` key.
[[nodiscard]] std::string retry_status_message(
    const cch::tui::KeybindingRegistry& keybindings,
    int attempt,
    int max_attempts,
    int seconds);

/// pi `CompactionStatusIndicator` message: manual `Compacting context...`,
/// threshold `Auto-compacting...`, overflow `Context overflow detected,
/// Auto-compacting...` — each with the interrupt-cancel hint.
[[nodiscard]] std::string compaction_status_message(
    const cch::tui::KeybindingRegistry& keybindings,
    std::string_view reason);

/// pi `CustomEditor.renderTopBorder` (embedWorkingStatus): the editor's top
/// border line carrying the embedded status indicator — `── ` plus the status
/// row, the `↑ N more` overflow label centered in the trailing rule when it
/// fits, spinner-only when the status cannot fit the width. `border_style`
/// styles the border runs (pi `this.borderColor`); nullopt falls back to the
/// default border line the editor computes (pi `super.renderTopBorder`).
[[nodiscard]] support::Expected<std::optional<std::string>> embedded_status_top_border(
    StatusIndicator& indicator,
    cch::tui::TextStyleHook& border_style,
    std::size_t width,
    std::size_t hidden_line_count);

} // namespace cch::coding_agent::tui
