#pragma once

// Native TUI startup resource loading (#506 extraction from the
// InteractiveState monolith): the assembled application keybinding catalog
// (pi's shared `KeybindingsManager` surface), the global-scope settings
// manager, and the boot theme controller (pi `InteractiveThemeController`).
// The interactive engine composes these at start and re-catalogs on `/reload`.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/ThemeController.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

/// Startup diagnostics surfaced in the chat after the view binds (pi
/// `renderInitialMessages` diagnostic lines).
struct InteractiveStartupDiagnostics {
    std::vector<SessionDiagnostic> session;
    std::vector<KeybindingDiagnostic> keybindings;
    /// Theme parse/collision diagnostics from the boot session's theme
    /// discovery (pi `resource-loader.ts` `getThemes` diagnostics).
    std::vector<ResourceDiagnostic> themes;
};

/// The assembled main-editor keybinding action-id list (pi's shared
/// `KeybindingsManager` catalog surface), shared by the startup catalog and
/// the `/reload` re-catalog (#418). `clipboard_paste_available` adds the
/// `app.clipboard.pasteImage` action.
[[nodiscard]] std::vector<std::string> assemble_keybinding_actions(bool clipboard_paste_available);

/// Load the application keybinding registry for one assembled action list
/// (pi `KeybindingsManager.create` + `reload`, ADR 0035).
[[nodiscard]] support::Expected<KeybindingsManagerResult> load_app_keybinding_manager(
    const std::filesystem::path& agent_config_directory,
    const std::vector<std::string>& actions);

/// The Native TUI reads only the global settings scope (the theme is
/// global-only) and writes selections surgically through the two-scope
/// manager with the project scope untrusted. A global-scope load error fails
/// startup.
[[nodiscard]] support::Expected<coding_agent::SettingsManager> create_interactive_settings_manager(
        const std::filesystem::path& agent_config_directory);

/// The boot theme controller's report sinks (pi `InteractiveThemeController`
/// `showError`/`onChanged`, the color-query result, and the mode-2031
/// report). The color and scheme sinks run on the TUI query/input thread and
/// must post their updates to the interactive executor.
struct InteractiveThemeHooks {
    std::move_only_function<void(std::string)> show_error{nullptr};
    std::move_only_function<void()> on_changed{nullptr};
    std::move_only_function<void(cch::tui::TerminalColors)> on_terminal_colors{nullptr};
    std::move_only_function<void(TerminalTheme)> on_terminal_color_scheme{nullptr};
};

/// pi interactive-mode ctor (`setRegisteredThemes` + the
/// `InteractiveThemeController`): the controller boots from the global-scope
/// theme setting (automatic pairs resolve against the terminal appearance;
/// the unsupported generated `system` theme maps to the appearance-selected
/// built-in, #991) and owns the live palette every component renders
/// through. `apply_from_settings` queries the terminal's colors and enables
/// mode 2031 for automatic values. Registered themes arrive with the boot
/// session (`set_registered_themes` at bind). The settings manager reference
/// must outlive the controller.
[[nodiscard]] std::unique_ptr<ThemeController> make_interactive_theme_controller(
        const std::filesystem::path& agent_config_directory,
        coding_agent::SettingsManager& settings_manager,
        cch::tui::TerminalColorCapability color_capability,
        cch::tui::ITui& root,
        InteractiveThemeHooks hooks);

} // namespace cch::coding_agent::tui
