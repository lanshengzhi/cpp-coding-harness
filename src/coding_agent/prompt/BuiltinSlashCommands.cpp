#include "BuiltinSlashCommands.hpp"

namespace cch::coding_agent::prompt {

const std::vector<BuiltinSlashCommand>& builtin_slash_commands() {
    // Compatibility baseline: pi 83114817,
    // packages/coding-agent/src/core/slash-commands.ts (ADR 0036 G4: the
    // 17 Supported entries of the 22-command catalog, pi-verbatim strings;
    // "/export" "/import" "/share" "/changelog" "/clone", "/debug", and the
    // easter eggs are Deferred with no surface).
    static const std::vector<BuiltinSlashCommand> kCommands{
        {"settings", "Open settings menu", {}},
        {"model", "Select model (opens selector UI)", "<provider/model>"},
        {"scoped-models", "Enable/disable models for Ctrl+P cycling", {}},
        {"copy", "Copy last agent message to clipboard", {}},
        {"name", "Set session display name", {}},
        {"session", "Show session info and stats", {}},
        {"hotkeys", "Show all keyboard shortcuts", {}},
        {"fork", "Create a new fork from a previous user message", {}},
        {"tree", "Navigate session tree (switch branches)", {}},
        {"trust", "Save project trust decision for future sessions", {}},
        {"login", "Configure provider authentication", "<provider>"},
        {"logout", "Remove provider authentication", {}},
        {"new", "Start a new session", {}},
        {"compact", "Manually compact the session context", {}},
        {"resume", "Resume a different session", {}},
        // pi "/reload": "Reload keybindings, extensions, skills, prompts,
        // themes, and context files" — the C++ subset drops "extensions"
        // (no extensions surface, ADR 0036 G4).
        {"reload", "Reload keybindings, skills, prompts, themes, and context files", {}},
        // pi "/quit": `Quit ${APP_NAME}` with the C++ binary's own identity.
        {"quit", "Quit cch", {}},
    };
    return kCommands;
}

} // namespace cch::coding_agent::prompt
