#pragma once

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Keys.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

class LiveTheme;

/// The Pending Elicitation prompt surface both mode dialogs share: the
/// interface the flow controller holds, the answer it delivers, and the three
/// controls that produce it. One file, so a control is keyed and spelled the
/// same way in URL mode and in form mode.

/// The user's disposition of one Pending Elicitation, delivered once. The
/// form values are the form mode's half of the same answer and are empty for
/// URL mode, for a decline, and for a cancel: a question the user refused is
/// an answer with no content, and content invented for it would be data the
/// user did not supply.
using McpElicitationAnswerSink = std::move_only_function<
        void(McpElicitationAction action, std::string elicitation_id, McpElicitationFormValues values)>;
/// Ask the host to open a presented address in the platform browser. The
/// dialog never launches anything itself: the action travels the host's
/// generation-checked action seam, so a dialog belonging to a retired session
/// cannot open a browser.
using McpElicitationOpenBrowserSink = std::move_only_function<void(std::string url)>;
/// pi's `ui.requestRender` after component-internal state changed; coalescible
/// and safe to call from any thread.
using McpElicitationInvalidateSink = std::move_only_function<void()>;

/// The one Pending Elicitation dialog the flow controller holds, in either
/// mode. Both dialogs are prompt-slot components, and the controller's whole
/// job with one is to know that a question is on screen and to be able to
/// take it down again when the host closes.
///
/// Withdrawing is not answering: a question being torn down with its session
/// has no answer to give, and a dialog still holding its answer sink would be
/// able to answer a call that is gone.
class McpElicitationPrompt {
public:
    McpElicitationPrompt(const McpElicitationPrompt&) = delete;
    McpElicitationPrompt& operator=(const McpElicitationPrompt&) = delete;
    McpElicitationPrompt(McpElicitationPrompt&&) = delete;
    McpElicitationPrompt& operator=(McpElicitationPrompt&&) = delete;
    virtual ~McpElicitationPrompt() = default;

    virtual void withdraw() = 0;

protected:
    McpElicitationPrompt() = default;
};

/// The three answers a Pending Elicitation can end in. Both dialogs read this
/// one table, so a control is keyed and spelled the same way in URL mode and
/// in form mode, and the hint line renders the key that is actually bound.
enum class McpElicitationControl { Done, Decline, Cancel };

/// The key identifier one control is on: a `tui.*` action id where the
/// built-in table has one, and a literal key where it does not.
[[nodiscard]] std::string_view mcp_elicitation_control_key(McpElicitationControl control) noexcept;

/// Whether `key` is this control. Decline is `alt+d` and not `d` because a
/// form field must be able to take a `d` for itself, and a control a user
/// cannot reach while a field has focus is not a control.
[[nodiscard]] bool mcp_elicitation_control_matched(
        const cch::tui::KeyEvent& key,
        const cch::tui::KeybindingRegistry& keybindings,
        McpElicitationControl control);

/// One control as the dialog's hint line names it, so the render and the test
/// read one spelling of the control set.
[[nodiscard]] std::string mcp_elicitation_control_hint(const LiveTheme& theme,
        const cch::tui::KeybindingRegistry& keybindings,
        McpElicitationControl control,
        std::string_view description);

} // namespace cch::coding_agent::tui
