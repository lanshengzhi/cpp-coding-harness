#pragma once

#include "coding_agent/tui/McpElicitationForm.hpp"
#include "coding_agent/tui/McpElicitationPrompt.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// The form-mode Pending Elicitation dialog (issue #846; spec #833 story 26).
///
/// It renders the fields the Upstream's JSON Schema declares, reads the
/// user's typing into them, and refuses to answer until every field
/// validates. Nothing invalid is ever sent: a rejected value produces an
/// inline error under its own field and the dialog stays up, because a form
/// that answered with what it could not parse is the Upstream's problem to
/// discover rather than the user's.
///
/// One `cch::tui::Input` and a focus index, the way `LoginDialogComponent`
/// has one input and one slot: every keypress is the `Input`'s to handle, and
/// the focus index is what says which field's value it currently holds. Tab,
/// Shift-Tab, and the up/down keys move between fields; the same
/// `McpElicitationControl` table every dialog reads ends the question.
///
/// The schema is untrusted input, and it is rendered as such: `read_form_schema`
/// holds it to the bounds in `mcp_form_bound`, and a field count, a label
/// width, and a nesting depth that the layout cannot carry are reported on
/// screen rather than drawn.
class McpElicitationFormDialog final : public cch::tui::Component,
                                       public cch::tui::InputHandler,
                                       public cch::tui::Focusable,
                                       public McpElicitationPrompt {
public:
    McpElicitationFormDialog(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            McpPendingElicitation request,
            McpElicitationAnswerSink on_answer,
            McpElicitationInvalidateSink on_invalidate = {});
    McpElicitationFormDialog(const McpElicitationFormDialog&) = delete;
    McpElicitationFormDialog& operator=(const McpElicitationFormDialog&) = delete;
    McpElicitationFormDialog(McpElicitationFormDialog&&) = delete;
    McpElicitationFormDialog& operator=(McpElicitationFormDialog&&) = delete;
    ~McpElicitationFormDialog() override = default;

    /// The Server Id whose question is on screen, empty once the dialog has
    /// been withdrawn.
    [[nodiscard]] std::string server_id() const;

    /// The field with focus, as the dialog's own index into the schema's
    /// fields. A form with no rendered field has no focused field.
    [[nodiscard]] std::optional<std::size_t> focused_field() const;

    /// The text currently held for one field, as the dialog holds it.
    [[nodiscard]] std::string field_value(std::size_t index) const;

    /// The inline error for one field, empty when it has none.
    [[nodiscard]] std::string field_error(std::size_t index) const;

    /// Withdraw the dialog **without answering**, as session Close and a
    /// stopped wait do. This is not the Cancel control: see
    /// `McpElicitationPrompt`.
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
    /// Settle the dialog with `action`, and with the form's values when the
    /// action is an accept. The first settlement wins.
    void settle(McpElicitationAction action, McpElicitationFormValues values);

    /// Retire the dialog, optionally delivering `action` first. One place
    /// settles and one place withdraws.
    void retire(std::optional<McpElicitationAction> action, McpElicitationFormValues values);

    /// Copy the `Input`'s current value into the focused field. Every key the
    /// `Input` handled leaves its value there, and a field switch would
    /// otherwise lose it.
    void store_focused();

    /// Move focus by `delta` fields, storing what the `Input` holds first.
    void move_focus(int delta);

    /// Validate every field. Returns the answer's values when every field
    /// passes, or nothing after recording the first failure under its own
    /// field and moving focus there — an unanswerable form never leaves the
    /// dialog, and nothing invalid is ever sent. The caller settles, so the
    /// sink runs with no lock held.
    [[nodiscard]] std::optional<McpElicitationFormValues> submit();

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    McpPendingElicitation request_;
    McpElicitationAnswerSink on_answer_;
    McpElicitationInvalidateSink on_invalidate_;
    /// The schema as this build reads it, decoded once when the dialog is
    /// built: the schema does not change under a question the user is
    /// answering, and re-reading it per frame would re-parse a hostile
    /// document on every tick.
    McpElicitationForm form_;
    /// The persistent single-line editor. It holds the focused field's value
    /// and nothing else; `values_` holds the rest.
    cch::tui::Input input_;
    std::vector<std::string> values_{};
    std::vector<std::string> errors_{};
    std::size_t focused_field_{0};
    mutable std::mutex mutex_;
    bool settled_{false};
    bool focused_{false};
    /// The focused field's input row from the last render, reported through
    /// the focus lifecycle.
    std::optional<cch::tui::CursorPosition> cursor_cache_{};
};

} // namespace cch::coding_agent::tui
