#pragma once

// Synchronous slash-command presentation effects for the Native TUI (pi
// `handleSessionCommand`/`handleHotkeysCommand`/`/help` subsets): the
// session-info chat block, the hotkeys chat block, and the help text.
// Extraction #506 from the pre-split interactive monolith; the engine
// owns session/view mutation and applies these values.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include <string>
#include <string_view>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::tui {
class KeybindingRegistry;
} // namespace cch::tui

namespace cch::coding_agent::tui {

/// pi `handleSessionCommand`: the Session Info chat block over the session
/// name, file, id, message counts, and token totals (pi `getSessionStats`
/// shape; the C++ subset renders the data the session exposes).
/// Workspace/provider/model are not pi fields and are intentionally absent
/// (strict subset).
[[nodiscard]] std::string format_session_info(const coding_agent::AgentSession& session);

/// pi `/help` chat text: the available built-in commands and keybindings
/// list rendered as one frontend message.
inline constexpr std::string_view kHelpCommandText =
    "Available commands:\n"
    "/clear /new /quit /exit /q /copy /session /hotkeys /settings\n"
    "/help /commands /name /model /models /scoped-models /thinking\n"
    "/login /logout /resume /fork /tree /reload /compact /trust";

/// pi `handleHotkeysCommand`: the Keyboard Shortcuts chat block over the
/// effective registry. Sections and row meanings follow pi's hardcoded
/// Navigation/Editing/Other tables (plus the `/`, `!`, `!!` rows);
/// unassembled or unbound actions render as `Unbound` like `key_hint`.
[[nodiscard]] std::string format_hotkeys_text(const cch::tui::KeybindingRegistry& registry);

} // namespace cch::coding_agent::tui
