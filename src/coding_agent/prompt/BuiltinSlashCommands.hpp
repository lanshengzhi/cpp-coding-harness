#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::prompt {

/// One Supported built-in slash command autocomplete entry (pi
/// `BuiltinSlashCommand`, `core/slash-commands.ts`).
struct BuiltinSlashCommand {
    std::string_view name;
    std::string_view description;
    std::string_view argument_hint{};
};

/// The app layer's Supported built-in slash commands for autocomplete: the
/// 18 Supported entries of pi's 22-command `BUILTIN_SLASH_COMMANDS` catalog
/// (`pi:packages/coding-agent/src/core/slash-commands.ts` at `f07218c4`, ADR
/// 0036 G4) with pi's verbatim names/descriptions/argument hints. The
/// Deferred slashes (`/export` `/import` `/share` `/changelog` `/clone`),
/// `/debug`, and the easter eggs are absent from autocomplete, and a typed
/// Deferred name is submitted as an ordinary Agent Prompt: the router passes
/// every unrecognized token through, matching pi's fall-through (issue #792).
/// The `/reload` description drops "extensions" (no extensions
/// surface), and `/quit` uses Pike's identity for pi's
/// `Quit ${APP_NAME}`. Router-only spellings (`/clear`, `/help`, `/commands`,
/// `/exit`, `/q`, `/models`) are offered by the palette from the router's
/// spelling table, not from this catalog.
[[nodiscard]] const std::vector<BuiltinSlashCommand>& builtin_slash_commands();

} // namespace cch::coding_agent::prompt
