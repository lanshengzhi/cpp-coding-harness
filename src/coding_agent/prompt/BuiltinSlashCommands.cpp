#include "BuiltinSlashCommands.hpp"

namespace cch::coding_agent::prompt {

const std::vector<BuiltinSlashCommand>& builtin_slash_commands() {
    // Compatibility baseline: pi 83114817,
    // packages/coding-agent/src/core/slash-commands.ts (ADR 0036 G4:
    // pi-verbatim strings; 17 of that catalog's 22 entries were ported, and
    // upstream has since added "thinking" — ported in #791 — and "bug" —
    // not-applicable, #793 — so f07218c4 carries 24 entries and this catalog
    // 18). The names with no surface: "/share" "/changelog"
    // "/clone" (Deferred), the hidden "/debug" developer entry,
    // "/bug", and the easter eggs. "/export" and "/import" are wired for
    // real (JSONL transcript + pi-faithful HTML default, and the JSONL
    // session import, #1044).
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
            {"thinking", "Set thinking level", "<level>"},
            {"trust", "Save project trust decision for future sessions", {}},
            {"new", "Start a new session", {}},
            {"compact", "Manually compact the session context", {}},
            {"resume", "Resume a different session", {}},
            // pi "/reload": "Reload keybindings, extensions, skills, prompts,
            // themes, and context files" — the C++ subset drops "extensions"
            // (no extensions surface, ADR 0036 G4).
            {"reload", "Reload keybindings, skills, prompts, themes, and context files", {}},
            // pi "/quit": `Quit ${APP_NAME}` with the C++ binary's own identity.
            {"quit", "Quit pike", {}},
            // pi `extensions/mcp/index.ts` `registerCommand("mcp")`
            // description, verbatim; the argument hint is the #884 wiring
            // lane's spelling of pi's `login [server]` et al.
            {"mcp",
                    "Manage MCP servers: sign in, reconnect, enable or disable, and change exposure",
                    "[action] [server]"},
            // pi v1.3.0 "/export": HTML is the default lane; a ".jsonl"
            // destination takes the transcript lane instead (#1044).
            {"export", "Export session (HTML default, or specify path: .html/.jsonl)", "[path]"},
            // pi v1.3.0 "/import": "Import and resume a session from a JSONL
            // file" — the copy lands in the session directory and the
            // imported session becomes the current one (#1044).
            {"import", "Import and resume a session from a JSONL file", "[path]"},
    };
    return kCommands;
}

} // namespace cch::coding_agent::prompt
