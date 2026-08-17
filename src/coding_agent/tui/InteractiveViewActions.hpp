#pragma once

// ── Main-screen view-to-state action seam (ADR 0040 shape) ──────────────────
//
// The Native TUI main-screen composition (`InteractiveView`) emits one closed
// passive `ViewAction` value to the owning `InteractiveState` through one
// move-only `ViewActionSink` (the same deepening that ADR 0040 recorded for
// the outer `TuiActionVariant` host seam, applied to the component seam).
// The sink is exception-free: it carries a `noexcept` function type and
// returns `support::ExpectedVoid`, so an ordinary failure is returned, never
// thrown. Render-state requests (`on_invalidate`) stay separate and may
// coalesce; this path never drops an admitted action.
//
// This is a repository-private `cch_coding_agent` implementation header: it
// is not part of an Owner Interface, is not installed, and is never exported.

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <variant>

namespace cch::coding_agent::tui {

/// Which submission kind an editor submission carries (pi's Enter vs
/// Alt+Enter distinction). Ordinary runs the full editor chain; FollowUp
/// queues directly when a run is active.
enum class InputSubmission { Ordinary, FollowUp };

/// One focused-editor submission payload (pi `submitPrompt` inputs).
struct EditorSubmissionRequest {
    std::string text;
    /// The editor revision at which the submission was sampled so a
    /// concurrent clear-pending-bash cannot restore stale text.
    std::size_t editor_revision{0};
};

/// One focused-editor interrupt payload (pi's `onEscape` chain inputs).
struct EditorInterruptRequest {
    std::string pending_bash_text;
    std::size_t editor_revision{0};
    bool pending_bash{false};
};

/// The model-cycle direction (pi `app.model.cycleForward` /
/// `app.model.cycleBackward`).
enum class ModelCycleDirection { Forward, Backward };

// ── Closed main-screen action alternatives ─────────────────────────────────

/// Submit an editor submission through the full editor chain (ordinary) or
/// queue it as follow-up input (follow-up while a run is active).
struct SubmitAction {
    EditorSubmissionRequest request;
    InputSubmission submission{InputSubmission::Ordinary};
};

/// Paste image (text fallback) from the system clipboard (pi
/// `app.clipboard.pasteImage`).
struct ClipboardPasteAction {};

/// Restore queued steering/follow-up messages to the editor (pi
/// `app.message.dequeue`).
struct DequeueAction {};

/// Interrupt the current activity per pi's onEscape precedence
/// (`app.interrupt`).
struct InterruptAction {
    EditorInterruptRequest request;
};

/// Exit the interactive mode when the editor is empty (pi `app.exit`).
struct ExitAction {};

/// Cycle to the next/previous model (pi `app.model.cycleForward` /
/// `app.model.cycleBackward`).
struct CycleModelAction {
    ModelCycleDirection direction{ModelCycleDirection::Forward};
};

/// Open the model selector (pi `app.model.select`).
struct SelectModelAction {};

/// Cycle the thinking level (pi `app.thinking.cycle`).
struct CycleThinkingAction {};

/// Toggle thinking-block visibility (pi `app.thinking.toggle`).
struct ToggleThinkingAction {};

/// Open the in-session session selector (pi `app.session.resume`).
struct ResumeSessionAction {};

/// Open the user-message fork selector (pi `app.session.fork`).
struct ForkSessionAction {};

/// Start the new-session flow (pi `app.session.new`).
struct NewSessionAction {};

/// Copy the last message to the clipboard (pi `app.message.copy`).
struct CopyLastMessageAction {};

/// Open the session tree selector (pi `app.session.tree`).
struct OpenTreeSelectorAction {};

/// Suspend to background (pi `app.suspend`).
struct SuspendAction {};

/// Open the external editor over the expanded editor content (pi
/// `app.editor.external`).
struct ExternalEditorAction {};

/// One closed main-screen view action. Each alternative is an owned passive
/// payload; the owning `InteractiveState` performs the operation on its
/// serialized execution path.
using ViewAction = std::variant<
    SubmitAction,
    ClipboardPasteAction,
    DequeueAction,
    InterruptAction,
    ExitAction,
    CycleModelAction,
    SelectModelAction,
    CycleThinkingAction,
    ToggleThinkingAction,
    ResumeSessionAction,
    ForkSessionAction,
    NewSessionAction,
    CopyLastMessageAction,
    OpenTreeSelectorAction,
    SuspendAction,
    ExternalEditorAction>;

/// One move-only sink carrying closed main-screen view actions to the owning
/// `InteractiveState`'s serialized execution path. Ordinary failures are
/// returned through `ExpectedVoid`; the function type is `noexcept` (a
/// throwing sink violates the seam contract and is a Runtime invariant
/// violation). A null sink drops the action silently.
using ViewActionSink =
    std::move_only_function<support::ExpectedVoid(ViewAction) noexcept>;

} // namespace cch::coding_agent::tui
