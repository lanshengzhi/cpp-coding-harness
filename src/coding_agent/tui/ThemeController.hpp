#pragma once

#include "Theme.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// pi `TerminalTheme` (`theme.ts`): the dark/light terminal appearance used
/// by automatic theme selection.
enum class TerminalTheme {
    Dark,
    Light,
};

/// pi `detectColorFgBgTheme`: the last COLORFGBG field must be one or two
/// decimal digits in 0..15; indexes 0..6 and 8 are dark, 7 and 9..15 are
/// light. Any other value is undefined, so the reported scheme and then the
/// dark fallback decide the appearance.
[[nodiscard]] std::optional<TerminalTheme> terminal_theme_from_colorfgbg(std::string_view colorfgbg);

/// pi `detectTerminalBackgroundFromEnv`: the env-only default detection,
/// reading `COLORFGBG`.
[[nodiscard]] std::optional<TerminalTheme> detect_terminal_theme_from_env();

/// pi `resolveThemeSetting`: explicit names pass through, valid `light/dark`
/// pairs select by terminal appearance, and malformed slash settings are unset.
[[nodiscard]] std::optional<std::string> resolve_theme_setting(
    const std::optional<std::string>& setting,
    TerminalTheme terminal_theme);

/// One registered theme (pi `setRegisteredThemes`): the parsed theme plus
/// its source path (pi `Theme.sourcePath`; absent only for in-memory
/// instances, which the subset never creates) and source scope (pi
/// `SourceInfo.scope`, carried through `discover_themes` from the loader
/// document — the loaded-resources Themes grouping dimension, #418).
struct RegisteredTheme {
    ResolvedTheme theme;
    std::optional<std::filesystem::path> source_path{std::nullopt};
    /// pi `SourceInfo.scope` of the discovering source: `Project` (the
    /// trust-gated project themes directory), `User` (the agent config
    /// directory themes directory), or `Temporary` (an explicit `--theme`
    /// path, the "path" group).
    SourceScope scope{SourceScope::Project};
};

/// pi `InteractiveThemeController` `showError` sink.
using ThemeErrorSink = std::move_only_function<void(std::string)>;
/// pi `InteractiveThemeController` `onChanged` sink (pi
/// `updateEditorBorderColor`).
using ThemeChangedSink = std::move_only_function<void()>;
using ThemeTerminalColorsSink = std::move_only_function<void(cch::tui::TerminalColors)>;
using ThemeTerminalColorSchemeSink = std::move_only_function<void(TerminalTheme)>;
/// pi `settingsManager.getThemeSetting()` provider (the raw global-scope
/// value; slash automatic pairs read as unset).
using ThemeSettingProvider = std::move_only_function<std::optional<std::string>()>;
/// The pi `InteractiveThemeController` subset (`theme-controller.ts`) over
/// the `theme.ts` subset, owning the live palette every component renders
/// through (pi's module-global `theme` instance):
///
/// - Boot init resolves explicit or paired settings against COLORFGBG;
///   without a usable setting it boots the generated `system` theme, whose
///   indexed tier needs no reported colors.
/// - `apply_from_settings` applies the current selection, queries OSC 10/11/4
///   and DA1, consumes partial and late replies, and enables mode 2031 only
///   for automatic selection. Reported background appearance outranks live
///   scheme, COLORFGBG, and dark fallback.
/// - `apply_theme_name` replicates pi `applyThemeName`: the reserved
///   `system` name regenerates from the retained terminal colors; failure of
///   any other name replaces the palette with the generated system theme,
///   records it as active, and reports the verbatim message when
///   `show_error` is set.
/// - `preview` is pi `preview`: in-memory apply without committing or
///   changing the active name; success invalidates and requests a render.
/// - The system theme renders grayscale while the first color report is
///   pending (pi `markTerminalColorsPending`); the first
///   `apply_terminal_colors` ends that state.
/// - Every change invalidates the borrowed root and fires `on_changed`
///   (pi `notifyChanged` → `ui.invalidate()` + `onChanged`), which the
///   interactive host wires to the editor border refresh.
///
/// The theme file watcher remains outside #991/#1032 (frozen pi skips the
/// generated and built-in themes).
class ThemeController final {
public:
    /// `custom_themes_dir` is the user custom-themes directory
    /// (`<agent_config_directory>/themes`; empty skips the directory).
    /// `registered` are the resource-loader themes (pi `setRegisteredThemes`
    /// in the interactive-mode ctor). `theme_setting` reads the global-scope
    /// `theme` User Settings field.
    ThemeController(std::filesystem::path custom_themes_dir,
            std::vector<RegisteredTheme> registered,
            ThemeSettingProvider theme_setting,
            cch::tui::TerminalColorCapability color_capability,
            cch::tui::ITui& root,
            ThemeErrorSink show_error,
            ThemeChangedSink on_changed,
            ThemeTerminalColorsSink on_terminal_colors,
            ThemeTerminalColorSchemeSink on_terminal_color_scheme);
    ThemeController(ThemeController&&) = delete;
    ThemeController& operator=(ThemeController&&) = delete;
    ~ThemeController();

    ThemeController(const ThemeController&) = delete;
    ThemeController& operator=(const ThemeController&) = delete;

    /// pi `setRegisteredThemes`: replace the registered-theme map (called
    /// at boot bind and by `/reload`'s re-registration).
    void set_registered_themes(std::vector<RegisteredTheme> registered);

    /// The registered themes with their source scopes (pi
    /// `resourceLoader.getThemes().themes`), for the loaded-resources
    /// Themes section (#418).
    [[nodiscard]] const std::vector<RegisteredTheme>& registered_themes() const;

    /// pi `applyFromSettings`: apply the current preference and query terminal colors.
    void apply_from_settings();

    /// Apply terminal reports on the product executor (the TUI delivers them
    /// from its timer or input path). The first report ends the system
    /// theme's pending grayscale state (pi `setTerminalColors`).
    void apply_terminal_colors(cch::tui::TerminalColors colors);
    void apply_terminal_color_scheme(TerminalTheme scheme);

    /// Re-register exactly one listener after a TUI rebind; dispose detaches
    /// it and disables mode 2031 before terminal shutdown.
    /// Rebind the listener exactly once after starting a replacement renderer.
    void rebind_tui(cch::tui::ITui& root);
    void rebind_terminal_color_scheme_listener();
    /// Terminal disposal suppresses later reports, including reports already
    /// posted to the host executor before disposal. A disposed controller is
    /// not reusable; create a new controller for a new run.
    void dispose();

    /// pi `setThemeName` → `applyThemeName`; returns success.
    [[nodiscard]] bool set_theme_name(std::string_view name, bool show_error);

    /// pi `preview`: in-memory apply of one theme name or setting (slash
    /// values resolve to the active theme) without committing or changing
    /// the active name.
    void preview(std::string_view theme_setting_or_name);

    /// pi `getAvailableThemes`: builtins + custom directory + registered
    /// themes, deduped first-wins (builtin, then the custom directory scan,
    /// then registered) and sorted by name. Invalid custom-directory files
    /// are ignored here like pi; the resource loader reports them.
    [[nodiscard]] std::vector<std::string> available_theme_names() const;

    [[nodiscard]] std::string_view active_theme_name() const;
    [[nodiscard]] TerminalTheme terminal_theme() const;
    [[nodiscard]] const LiveTheme& live_theme() const;

    /// pi `getResolvedThemeColors`/`getThemeExportColors` source for the
    /// `/export` HTML lane: the committed active theme (the last
    /// successfully applied resolution, or the dark fallback after a failed
    /// apply — never an in-memory `preview`).
    [[nodiscard]] const ResolvedTheme& active_resolved_theme() const;
    /// The terminal-reported default colors (pi's `terminalColors`), used by
    /// the export layer to resolve `""` (terminal-default) theme tokens.
    [[nodiscard]] const std::optional<cch::tui::TerminalColors>& reported_terminal_colors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// pi `initTheme` subset: the boot theme init for all modes before the
/// no-model guard (pi main.ts). Loads the raw settings theme (pairs resolve
/// only in the interactive controller), the env-detected default or the
/// reserved generated `system` theme, falls back to the generated system
/// theme silently on failure, and resolves builtins and
/// `<agent_config_directory>/themes/<name>.json` — the palette every mode
/// boots with. The interactive controller re-inits from the same settings at
/// TUI construction (pi `InteractiveThemeController`), and the startup-TUI
/// host (pi `startup-ui.ts` `createStartupTui`) consumes the same init.
[[nodiscard]] ResolvedTheme init_boot_theme(
    const std::filesystem::path& agent_config_directory,
    const std::optional<std::string>& theme_setting);

/// pi `resource-loader.ts` `loadThemes` + `dedupeThemes` (parse step): the
/// loader collects theme documents (project `.pi/themes` trust-gated, user
/// `<agent_config_directory>/themes`, explicit `--theme` paths — discovered
/// before explicit, pi's merge order) and this function parses them
/// (warnings on failure) and dedupes by name first-wins with pi's `name
/// "<name>" collision` diagnostic carrying the winner/loser paths.
/// Registration follows through `ThemeController::set_registered_themes`.
struct ThemeDiscoveryResult {
    std::vector<RegisteredTheme> themes;
    std::vector<ResourceDiagnostic> diagnostics;
};

[[nodiscard]] ThemeDiscoveryResult discover_themes(
    std::vector<LoadedThemeResource> documents);

} // namespace cch::coding_agent::tui
