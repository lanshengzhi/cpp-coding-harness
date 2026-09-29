#include "coding_agent/tui/McpElicitationPrompt.hpp"

#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <string>
#include <string_view>

namespace cch::coding_agent::tui {
namespace {

/// The key identifier of a control that is not in the built-in table. There
/// is one: decline has to be reachable while a form field has focus, and
/// every unmodified printable key belongs to the field.
constexpr std::string_view kDeclineKey{"alt+d"};

[[nodiscard]] bool is_decline_key(const cch::tui::KeyEvent& key) {
    return key.alt && !key.ctrl && !key.shift && key.key == "d";
}

} // namespace

std::string_view mcp_elicitation_control_key(McpElicitationControl control) noexcept {
    switch (control) {
    case McpElicitationControl::Done:
        return "tui.select.confirm";
    case McpElicitationControl::Decline:
        return kDeclineKey;
    case McpElicitationControl::Cancel:
        return "tui.select.cancel";
    }
    return "tui.select.cancel";
}

bool mcp_elicitation_control_matched(
        const cch::tui::KeyEvent& key, const cch::tui::KeybindingRegistry& keybindings,
        McpElicitationControl control) {
    if (control == McpElicitationControl::Decline) {
        return is_decline_key(key);
    }
    return keybindings.matches(key, mcp_elicitation_control_key(control));
}

std::string mcp_elicitation_control_hint(const LiveTheme& theme,
        const cch::tui::KeybindingRegistry& keybindings, McpElicitationControl control,
        std::string_view description) {
    if (control == McpElicitationControl::Decline) {
        return raw_key_hint(theme, kDeclineKey, description);
    }
    return key_hint(theme, keybindings, mcp_elicitation_control_key(control), description);
}

} // namespace cch::coding_agent::tui
