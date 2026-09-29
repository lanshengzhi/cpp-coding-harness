#pragma once

#include "coding_agent/tui/McpElicitationPrompt.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
namespace cch::coding_agent::tui {

class LiveTheme;

/// One URL-mode Pending Elicitation, as the dialog shows it (issue #845;
/// spec #833 story 27). The projection row is the whole input: the dialog
/// names the Server Id so the user can tell which upstream is waiting, shows
/// the address, and offers the three answers.
struct McpUrlElicitationView {
    McpPendingElicitation request{};
};

/// The URL-mode Pending Elicitation dialog (issue #845; spec #833 story 27).
///
/// It renders the address the Upstream asked about, an open-browser action,
/// and explicit **Done**, **Decline**, and **Cancel** controls. Opening the
/// browser is deliberately not one of the controls: the user may visit the
/// page and still decline, and a dialog that treated a browser launch as an
/// answer would let an Upstream manufacture its own consent.
///
/// Every accessor is under one mutex, so the Runtime domain that opened the
/// dialog and the input thread that drives it never race, and a session that
/// is closed while the dialog is up can withdraw it (ADR 0051/0052).
class McpElicitationDialog final : public cch::tui::Component,
                                   public cch::tui::InputHandler,
                                   public cch::tui::Focusable,
                                   public McpElicitationPrompt {
public:
    McpElicitationDialog(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            McpUrlElicitationView view,
            McpElicitationAnswerSink on_answer,
            McpElicitationOpenBrowserSink on_open_browser,
            McpElicitationInvalidateSink on_invalidate = {});
    McpElicitationDialog(const McpElicitationDialog&) = delete;
    McpElicitationDialog& operator=(const McpElicitationDialog&) = delete;
    McpElicitationDialog(McpElicitationDialog&&) = delete;
    McpElicitationDialog& operator=(McpElicitationDialog&&) = delete;
    ~McpElicitationDialog() override = default;

    /// The Server Id whose approval page is on screen, empty once the dialog
    /// has been withdrawn.
    [[nodiscard]] std::string server_id() const;

    /// Open the presented address in the platform browser. A no-op once the
    /// dialog has been withdrawn: a dialog whose session is gone must not be
    /// able to launch anything.
    void open_browser();

    /// Withdraw the dialog **without answering**, as session Close and a
    /// stopped wait do. This is not the Cancel control: Cancel is an answer
    /// the Upstream is told about, while a withdrawal means there is no longer
    /// a call to answer, so inventing one would be answering a question
    /// nobody asked. The sinks are dropped, a later click is inert, and a
    /// second withdrawal is a no-op.
    void withdraw() override;

    /// Whether the question is still on screen.
    [[nodiscard]] bool live() const;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    /// Settle the dialog with `action` when it is still live. The first
    /// settlement wins and every later one is dropped, which is what makes a
    /// keypress racing a session close harmless.
    void settle(McpElicitationAction action);

    /// Retire the dialog, optionally delivering `action` first. One place
    /// settles and one place withdraws, so "the first settlement wins" is a
    /// single rule rather than two that could disagree.
    void retire(std::optional<McpElicitationAction> action);

    const LiveTheme& theme_;
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    McpUrlElicitationView view_;
    McpElicitationAnswerSink on_answer_;
    McpElicitationOpenBrowserSink on_open_browser_;
    McpElicitationInvalidateSink on_invalidate_;
    mutable std::mutex mutex_;
    bool settled_{false};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
