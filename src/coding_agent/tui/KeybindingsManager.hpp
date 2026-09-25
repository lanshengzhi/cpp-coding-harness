#pragma once

#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

enum class KeybindingDiagnosticSeverity {
    Warning,
};

struct KeybindingDiagnostic {
    KeybindingDiagnosticSeverity severity{KeybindingDiagnosticSeverity::Warning};
    std::string code{};
    std::string message{};
    std::string path{};
};

struct KeybindingsManagerRequest {
    std::filesystem::path agent_config_directory{};
    /// Concrete application actions supplied by the assembling frontend.
    /// Omitted application actions are unavailable and are never registered.
    std::vector<cch::tui::KeybindingDefinition> application_definitions{};
};

struct HotkeyHelpRow;

struct KeybindingsManagerResult {
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry{};
    std::shared_ptr<const std::vector<HotkeyHelpRow>> help{};
    std::vector<KeybindingDiagnostic> diagnostics{};
};

/// The app layer adopts pi's full 42-action `AppKeybindings` table plus the
/// product-added `app.thinking.save` action (43 total;
/// `pi:packages/coding-agent/src/core/keybindings.ts` at `83114817`, ADR
/// 0036): this returns baseline definitions only for the concrete application
/// action IDs selected by an assembling frontend, drawn from the full
/// catalog. Unknown IDs fail rather than creating placeholders.
[[nodiscard]] support::Expected<std::vector<cch::tui::KeybindingDefinition>>
app_keybinding_definitions(
    std::span<const std::string_view> assembled_action_ids);

/// Load exactly <Agent Config Directory>/keybindings.json and resolve one
/// startup registry (pi `KeybindingsManager.create` + `reload`, ADR 0035).
/// No pi state directory or project path is discovered.
[[nodiscard]] support::Expected<KeybindingsManagerResult> load_keybindings_manager(
    KeybindingsManagerRequest request);

// ── Registry help entries ────────────────────────────────────────────────
// These are the assembled-registry projection used by component consumers;
// the inline `/hotkeys` chat block is rendered by the single formatter in
// SlashCommandEffects so both surfaces share the same effective keys.

struct HotkeyHelpEntry {
    std::string id{};
    std::string keys{};
    std::string description{};
    std::string category{};
};

struct HotkeyHelpRow {
    std::string section{};
    std::string group{};
    std::string keys{};
    std::string description{};
    std::size_t order{};
};

/// One help entry per assembled registry entry (never a no-op binding).
[[nodiscard]] std::vector<HotkeyHelpEntry> hotkey_help_entries(
    const cch::tui::KeybindingRegistry& registry);

/// Help rows derived from the metadata on assembled registry entries. Keys are
/// looked up in that same immutable registry; known-but-unassembled actions
/// are diagnosed and omitted rather than fabricated as help rows.
[[nodiscard]] std::vector<HotkeyHelpRow> hotkey_help_rows(const cch::tui::KeybindingRegistry& registry);

/// `keys description` for one action, "Unbound" for an empty binding.
[[nodiscard]] std::string key_hint(
    const cch::tui::KeybindingRegistry& registry,
    std::string_view action_id,
    std::string_view description);

} // namespace cch::coding_agent::tui
