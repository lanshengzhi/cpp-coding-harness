#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

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

struct KeybindingsManagerResult {
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry{};
    std::vector<KeybindingDiagnostic> diagnostics{};
};

/// The app layer adopts pi's full 42-action `AppKeybindings` table
/// (`pi:packages/coding-agent/src/core/keybindings.ts` at `83114817`, ADR
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

// ── /hotkeys presentation (pi interactive-mode.ts `handleHotkeysCommand`
//    subset; renders the assembled subset only) ─────────────────────────────

struct HotkeyHelpEntry {
    std::string id{};
    std::string keys{};
    std::string description{};
    std::string category{};
};

/// One help entry per assembled registry entry (never a no-op binding).
[[nodiscard]] std::vector<HotkeyHelpEntry> hotkey_help_entries(
    const cch::tui::KeybindingRegistry& registry);

/// `keys description` for one action, "Unbound" for an empty binding.
[[nodiscard]] std::string key_hint(
    const cch::tui::KeybindingRegistry& registry,
    std::string_view action_id,
    std::string_view description);

/// The `/hotkeys` overlay content rendered from the assembled registry.
[[nodiscard]] std::unique_ptr<cch::tui::Component> make_hotkey_help_view(
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry);

} // namespace cch::coding_agent::tui
