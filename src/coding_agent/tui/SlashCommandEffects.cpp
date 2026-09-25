#include "SlashCommandEffects.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"

#include <cch/tui/Keybindings.hpp>

#include <format>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

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

std::string format_hotkeys_text(std::span<const HotkeyHelpRow> rows) {
    // The rows are resolved once by KeybindingsManager; this formatter only
    // lays them out. The only literals are the documented section names and
    // the three non-key command rows.
    std::string text = "Keyboard Shortcuts\n";
    constexpr std::string_view kSections[] = {"Navigation", "Editing", "Other"};
    for (const auto section : kSections) {
        text += "\n" + std::string{section} + "\n";
        for (const auto& row : rows) {
            if (row.section != section) continue;
            text += std::format("{}  {}\n", row.keys, row.description);
        }
        if (section == "Other") {
            text += "/  Slash commands\n";
            text += "!  Run bash command\n";
            text += "!!  Run bash command (excluded from context)\n";
        }
    }
    return text;
}

std::string format_hotkeys_text(const cch::tui::KeybindingRegistry& registry) {
    return format_hotkeys_text(hotkey_help_rows(registry));
}

} // namespace cch::coding_agent::tui
