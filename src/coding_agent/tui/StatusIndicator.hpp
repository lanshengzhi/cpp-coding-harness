#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
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
    /// frame and countdown tick (pi's Loader `requestRender`).
    StatusIndicator(
        Kind kind,
        const LiveTheme& theme,
        cch::tui::RenderRequestSink request_render,
        std::string message);

    [[nodiscard]] Kind kind() const { return kind_; }

    /// pi `StatusIndicator.setMessage` (the retry countdown rewrites the
    /// message on every tick).
    void set_message(std::string message);

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    Kind kind_;
    const LiveTheme& theme_; // must outlive this component.
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

} // namespace cch::coding_agent::tui
