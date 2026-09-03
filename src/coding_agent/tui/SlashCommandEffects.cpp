#include "SlashCommandEffects.hpp"

#include "coding_agent/AgentSession.hpp"

#include <cch/tui/Keybindings.hpp>

#include <format>
#include <span>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {
namespace {

// pi `handleHotkeysCommand` rows that combine several actions on one table
// line; the formatter joins their effective keys with `/`.
constexpr std::string_view kCursorMoveIds[] = {
        "tui.editor.cursorUp", "tui.editor.cursorDown", "tui.editor.cursorLeft", "tui.editor.cursorRight"};
constexpr std::string_view kWordMoveIds[] = {"tui.editor.cursorWordLeft", "tui.editor.cursorWordRight"};
constexpr std::string_view kPageIds[] = {"tui.editor.pageUp", "tui.editor.pageDown"};
constexpr std::string_view kModelCycleIds[] = {"app.model.cycleForward", "app.model.cycleBackward"};

// pi `formatKeys` empty case plus the pike `key_hint` convention: an absent
// or unbound action renders as `Unbound` rather than an empty span.
[[nodiscard]] std::string display_keys(const cch::tui::KeybindingRegistry& registry, std::string_view id) {
    const auto text = registry.key_text(id);
    return text.empty() ? "Unbound" : text;
}

// One combined row: the effective keys of each action joined with `/`, or
// `Unbound` when none of the actions is bound.
[[nodiscard]] std::string display_keys_many(
        const cch::tui::KeybindingRegistry& registry, std::span<const std::string_view> ids) {
    std::string text;
    for (const auto id : ids) {
        const auto keys = registry.key_text(id);
        if (keys.empty()) continue;
        if (!text.empty()) text.push_back('/');
        text += keys;
    }
    return text.empty() ? "Unbound" : text;
}

} // namespace

std::string format_session_info(const coding_agent::AgentSession& session) {
    // pi `handleSessionCommand` shape: Name (when set), File, ID, the
    // Messages breakdown, and the Tokens totals. Workspace/provider/model
    // are not pi fields and are intentionally absent (strict subset).
    const auto name = session.session_name();
    const auto path = session.session_path();
    const auto stats = session.session_stats();
    std::string info = "Session Info\n\n";
    if (name && !name->empty()) {
        info += std::format("Name: {}\n", *name);
    }
    info += std::format("File: {}\n", path ? path->string() : std::string{"In-memory"});
    info += std::format("ID: {}\n\n", session.session_id());
    info += "Messages\n";
    info += std::format("Total: {}\n", stats.total_messages);
    info += std::format("User: {}\n", stats.user_messages);
    info += std::format("Assistant: {}\n", stats.assistant_messages);
    info += std::format("Tools: {} calls, {} results\n", stats.tool_calls, stats.tool_results);
    info += "\nTokens\n";
    // pi: "Input" is the full prompt volume (input + cached + written);
    // the C++ subset renders the provider-independent split.
    const auto prompt_tokens = stats.input_tokens + stats.cache_read + stats.cache_write;
    info += std::format("Input: {}\n", prompt_tokens);
    if (prompt_tokens > 0 && (stats.cache_read > 0 || stats.cache_write > 0)) {
        info += std::format("Cached: {}\n", stats.cache_read);
        info += std::format("Uncached: {}\n", stats.input_tokens + stats.cache_write);
    }
    info += std::format("Output: {}\n", stats.output_tokens);
    info += std::format("Total: {}\n", prompt_tokens + stats.output_tokens);
    return info;
}

std::string format_hotkeys_text(const cch::tui::KeybindingRegistry& registry) {
    // pi `handleHotkeysCommand` sections and row meanings; keys are the
    // effective registry values so user overrides show up verbatim.
    std::string text = "Keyboard Shortcuts\n";
    text += "\nNavigation\n";
    text += std::format("{}  Move cursor / browse history\n", display_keys_many(registry, kCursorMoveIds));
    text += std::format("{}  Move by word\n", display_keys_many(registry, kWordMoveIds));
    text += std::format("{}  Start of line\n", display_keys(registry, "tui.editor.cursorLineStart"));
    text += std::format("{}  End of line\n", display_keys(registry, "tui.editor.cursorLineEnd"));
    text += std::format("{}  Jump forward to character\n", display_keys(registry, "tui.editor.jumpForward"));
    text += std::format("{}  Jump backward to character\n", display_keys(registry, "tui.editor.jumpBackward"));
    text += std::format("{}  Scroll by page\n", display_keys_many(registry, kPageIds));
    text += "\nEditing\n";
    text += std::format("{}  Send message\n", display_keys(registry, "tui.input.submit"));
    text += std::format("{}  New line\n", display_keys(registry, "tui.input.newLine"));
    text += std::format("{}  Delete word backwards\n", display_keys(registry, "tui.editor.deleteWordBackward"));
    text += std::format("{}  Delete word forwards\n", display_keys(registry, "tui.editor.deleteWordForward"));
    text += std::format("{}  Delete to start of line\n", display_keys(registry, "tui.editor.deleteToLineStart"));
    text += std::format("{}  Delete to end of line\n", display_keys(registry, "tui.editor.deleteToLineEnd"));
    text += std::format("{}  Paste the most-recently-deleted text\n", display_keys(registry, "tui.editor.yank"));
    text += std::format(
            "{}  Cycle through the deleted text after pasting\n", display_keys(registry, "tui.editor.yankPop"));
    text += std::format("{}  Undo\n", display_keys(registry, "tui.editor.undo"));
    text += "\nOther\n";
    text += std::format("{}  Path completion / accept autocomplete\n", display_keys(registry, "tui.input.tab"));
    text += std::format("{}  Cancel autocomplete / abort streaming\n", display_keys(registry, "app.interrupt"));
    text += std::format("{}  Clear editor (first) / exit (second)\n", display_keys(registry, "app.clear"));
    text += std::format("{}  Exit (when editor is empty)\n", display_keys(registry, "app.exit"));
    text += std::format("{}  Suspend to background\n", display_keys(registry, "app.suspend"));
    text += std::format("{}  Cycle thinking level\n", display_keys(registry, "app.thinking.cycle"));
    text += std::format("{}  Cycle models\n", display_keys_many(registry, kModelCycleIds));
    text += std::format("{}  Open model selector\n", display_keys(registry, "app.model.select"));
    text += std::format("{}  Toggle tool output expansion\n", display_keys(registry, "app.tools.expand"));
    text += std::format("{}  Toggle thinking block visibility\n", display_keys(registry, "app.thinking.toggle"));
    text += std::format("{}  Edit message in external editor\n", display_keys(registry, "app.editor.external"));
    text += std::format("{}  Copy last assistant message\n", display_keys(registry, "app.message.copy"));
    text += std::format("{}  Queue follow-up message\n", display_keys(registry, "app.message.followUp"));
    text += std::format("{}  Restore queued messages\n", display_keys(registry, "app.message.dequeue"));
    text += std::format("{}  Paste image or text from clipboard\n", display_keys(registry, "app.clipboard.pasteImage"));
    text += "/  Slash commands\n";
    text += "!  Run bash command\n";
    text += "!!  Run bash command (excluded from context)\n";
    return text;
}

} // namespace cch::coding_agent::tui
