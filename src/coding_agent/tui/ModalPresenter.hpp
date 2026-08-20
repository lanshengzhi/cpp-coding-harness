#pragma once

#include <memory>
#include <string>

namespace cch::tui {
class Component;
class Overlay;
} // namespace cch::tui

namespace cch::coding_agent::tui {

/// The modal presentation seam for the Native TUI interactive flows (#501).
/// The flow controllers own coroutine orchestration and session state; the
/// presenter owns how overlays, prompt-slot replacements, and status text
/// reach the terminal, so a headless host can drive the flows against a
/// recording presenter without terminal emulation.
///
/// Thread contract: unless noted, methods are confined to the host executor.
/// Presentation is fire-and-forget value work; failures surface through the
/// host's own diagnostic path.
class ModalPresenter {
public:
    virtual ~ModalPresenter() = default;

    /// Show one modal overlay (pi's overlay stack: hotkey help, trust
    /// prompts). The presenter takes ownership and focuses the overlay.
    virtual void show_overlay(std::unique_ptr<cch::tui::Overlay> overlay) = 0;
    /// Close the active overlay when one is showing; a no-op otherwise.
    virtual void close_overlay() = 0;

    /// Swap the editor out of the prompt slot for a modal component (pi's
    /// `showSelector` editorContainer swap: the model, scoped-models,
    /// settings, and session selectors all render in the editor slot).
    virtual void replace_prompt_slot(std::shared_ptr<cch::tui::Component> component) = 0;
    /// Restore the editor into the prompt slot after a modal closes.
    virtual void restore_prompt_slot() = 0;

    /// pi `showStatus`: one dim status line in the chat.
    virtual void show_status(std::string text) = 0;
    /// pi `showError`: one diagnostic line in the chat.
    virtual void show_error(std::string text) = 0;

    /// Request one re-render after component-internal state changed (pi
    /// `ui.requestRender`); coalescible and safe to call from any thread.
    virtual void request_render() = 0;
    /// Mark the current frame dirty (executor-confined); the next render
    /// pass picks it up.
    virtual void invalidate() = 0;
};

} // namespace cch::coding_agent::tui
