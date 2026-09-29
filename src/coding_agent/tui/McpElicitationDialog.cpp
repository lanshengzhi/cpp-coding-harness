#include "coding_agent/tui/McpElicitationDialog.hpp"

#include "DynamicBorder.hpp"
#include "KeybindingHints.hpp"
#include "Theme.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// pi's OSC 8 hyperlink wrapper, the same spelling the login dialog uses for
/// the auth URL: a terminal that supports it makes the address clickable, and
/// one that does not shows the text unchanged.
[[nodiscard]] std::string hyperlink(std::string_view url, std::string_view text) {
    return "\x1b]8;;" + std::string{url} + "\x07" + std::string{text} + "\x1b]8;;\x07";
}

} // namespace

std::string_view McpElicitationDialog::control_key(Control control) noexcept {
    switch (control) {
    case Control::Done:
        return "tui.select.confirm";
    case Control::Decline:
        return "d";
    case Control::Cancel:
        return "tui.select.cancel";
    }
    return "tui.select.cancel";
}

McpElicitationDialog::McpElicitationDialog(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        McpUrlElicitationView view,
        McpElicitationAnswerSink on_answer,
        McpElicitationOpenBrowserSink on_open_browser,
        McpElicitationInvalidateSink on_invalidate)
    : theme_(theme), keybindings_(std::move(keybindings)), view_(std::move(view)), on_answer_(std::move(on_answer)),
      on_open_browser_(std::move(on_open_browser)), on_invalidate_(std::move(on_invalidate)) {}

std::string McpElicitationDialog::server_id() const {
    const std::scoped_lock lock(mutex_);
    return view_.request.server_id;
}

bool McpElicitationDialog::live() const {
    const std::scoped_lock lock(mutex_);
    return !settled_;
}

void McpElicitationDialog::open_browser() {
    std::string url;
    McpElicitationOpenBrowserSink sink;
    {
        const std::scoped_lock lock(mutex_);
        if (settled_) {
            return; // a withdrawn dialog must not be able to launch anything
        }
        url = view_.request.url;
        sink = on_open_browser_ ? std::move(on_open_browser_) : McpElicitationOpenBrowserSink{};
    }
    if (sink) {
        sink(std::move(url));
    }
}

void McpElicitationDialog::withdraw() { retire(std::nullopt); }

void McpElicitationDialog::settle(McpElicitationAction action) { retire(action); }

void McpElicitationDialog::retire(std::optional<McpElicitationAction> action) {
    std::string elicitation_id;
    McpElicitationAnswerSink sink;
    {
        const std::scoped_lock lock(mutex_);
        if (settled_) {
            return; // first settlement wins: a keypress racing a close is harmless
        }
        settled_ = true;
        elicitation_id = view_.request.elicitation_id;
        // A withdrawal drops the sinks as well: a dialog that is gone must not
        // be able to answer, or to launch a browser, on the way out.
        sink = action.has_value() && on_answer_ ? std::move(on_answer_) : McpElicitationAnswerSink{};
        on_open_browser_ = nullptr;
    }
    // The sink runs outside the lock: it answers the session, which posts back
    // into the very loop that took the lock.
    if (sink) {
        sink(*action, std::move(elicitation_id));
    }
    if (on_invalidate_) {
        on_invalidate_();
    }
}

cch::tui::InputAdmissionOutcome McpElicitationDialog::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    if (!cch::tui::carries_press_behavior(key)) {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }
    {
        const std::scoped_lock lock(mutex_);
        if (settled_) {
            return cch::tui::InputAdmissionOutcome::Consumed;
        }
    }
    if (keybindings_->matches(*key, control_key(Control::Done))) {
        settle(McpElicitationAction::Accept);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (keybindings_->matches(*key, control_key(Control::Cancel))) {
        settle(McpElicitationAction::Cancel);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (!key->ctrl && !key->alt && !key->shift && key->key == control_key(Control::Decline)) {
        settle(McpElicitationAction::Decline);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    if (!key->ctrl && !key->alt && !key->shift && key->key == "o") {
        // Opening the browser is an action, not an answer: the dialog stays up
        // and the question stays unanswered, because an Upstream must not be
        // able to manufacture consent by launching a page.
        open_browser();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    return cch::tui::InputAdmissionOutcome::Unhandled;
}

support::Expected<cch::tui::RenderResult> McpElicitationDialog::render(std::size_t width) {
    std::scoped_lock lock(mutex_);
    const auto& request = view_.request;
    cch::tui::RenderResult result;
    const auto append_text = [&result, width](const std::string& content) -> support::ExpectedVoid {
        cch::tui::Text line(content, 1, 0);
        auto rendered = line.render(width);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        for (auto& line_text : rendered->lines) {
            result.lines.push_back(std::move(line_text));
        }
        return {};
    };

    // pi's composition: border / accent title / content / border.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    auto rendered_border = top_border.render(width);
    if (!rendered_border) {
        return std::unexpected(rendered_border.error());
    }
    for (auto& line : rendered_border->lines) {
        result.lines.push_back(std::move(line));
    }
    if (auto appended = append_text(theme_.foreground(
                ThemeToken::Accent, "\x1b[1m" + std::string{"Approve an Upstream request"} + "\x1b[22m"));
            !appended) {
        return std::unexpected(appended.error());
    }
    // The Server Id is on screen: the user is authorizing one upstream's work,
    // and an approval page with no server named is not consent anyone can give.
    if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, "Upstream: " + request.server_id)); !appended) {
        return std::unexpected(appended.error());
    }
    if (!request.tool_name.empty()) {
        if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, "Tool: " + request.tool_name)); !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (!request.message.empty()) {
        if (auto appended = append_text(request.message); !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (!request.url.empty()) {
        if (auto appended = append_text(theme_.foreground(ThemeToken::Accent, hyperlink(request.url, request.url)));
                !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (auto appended = append_text(theme_.foreground(ThemeToken::Dim, "o open browser (not an answer)")); !appended) {
        return std::unexpected(appended.error());
    }
    // The three answers are named on screen, not implied by a keybinding the
    // user has to know: Done, Decline, and Cancel are different decisions.
    const auto controls = key_hint(theme_, *keybindings_, control_key(Control::Done), "done") + "  " +
                          raw_key_hint(theme_, control_key(Control::Decline), "decline") + "  " +
                          key_hint(theme_, *keybindings_, control_key(Control::Cancel), "cancel");
    if (auto appended = append_text(controls); !appended) {
        return std::unexpected(appended.error());
    }

    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    auto rendered_bottom = bottom_border.render(width);
    if (!rendered_bottom) {
        return std::unexpected(rendered_bottom.error());
    }
    for (auto& line : rendered_bottom->lines) {
        result.lines.push_back(std::move(line));
    }
    return result;
}

void McpElicitationDialog::invalidate() {}

void McpElicitationDialog::set_focused(bool focused) {
    const std::scoped_lock lock(mutex_);
    focused_ = focused;
}

bool McpElicitationDialog::focused() const {
    const std::scoped_lock lock(mutex_);
    return focused_;
}

std::optional<cch::tui::CursorPosition> McpElicitationDialog::cursor_location() const {
    return std::nullopt; // no text entry: the three answers are keys
}

} // namespace cch::coding_agent::tui
