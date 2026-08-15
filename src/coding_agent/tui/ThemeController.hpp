#pragma once

#include "Theme.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/tui/Tui.hpp>
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
/// by the default theme selection. The subset derives it from COLORFGBG
/// only; OSC 11/DSR queries, the automatic `light/dark` pair, auto-sync,
/// and the file watcher are Deferred with no placeholder.
enum class TerminalTheme {
    Dark,
    Light,
};

/// pi `detectTerminalBackgroundFromEnv` result subset: the detected theme
/// plus the confidence that decides whether a detected default persists to
/// the User Settings (`applyFromSettings` high-confidence write). The OSC 11
/// "terminal background" source is absent.
struct TerminalThemeDetection {
    TerminalTheme theme{TerminalTheme::Dark};
    /// pi `confidence`: high when COLORFGBG carried a background index,
    /// low for the dark fallback.
    bool high_confidence{false};
};

/// pi `detectTerminalBackgroundFromEnv` pure subset: parse COLORFGBG
/// (`"fg;bg"` or more fields; the last field that parses to a 0..255 index
/// is the background), classify the ANSI index by relative luminance
/// (≥ 0.5 → light), and fall back to dark when no index is present.
[[nodiscard]] TerminalThemeDetection terminal_theme_from_colorfgbg(
    std::string_view colorfgbg);

/// pi `detectTerminalBackgroundFromEnv`: the env-only default detection,
/// reading `COLORFGBG`.
[[nodiscard]] TerminalThemeDetection detect_terminal_theme_from_env();

/// pi `resolveThemeSetting` subset: the automatic `light/dark` settings pair
/// (any value containing "/") reads as unset; other string values pass
/// through unchanged.
[[nodiscard]] std::optional<std::string> resolve_theme_setting(
    const std::optional<std::string>& setting,
    TerminalTheme terminal_theme);

[[nodiscard]] std::string_view terminal_theme_name(TerminalTheme theme);

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
/// pi `settingsManager.getThemeSetting()` provider (the raw global-scope
/// value; slash automatic pairs read as unset).
using ThemeSettingProvider = std::move_only_function<std::optional<std::string>()>;
/// pi `settingsManager.setTheme(themeSetting)` global-scope write, used by
/// `applyFromSettings`' high-confidence default persistence.
using ThemeSettingCommitter =
    std::move_only_function<support::ExpectedVoid(std::string_view)>;

/// The pi `InteractiveThemeController` subset (`theme-controller.ts`) over
/// the `theme.ts` subset, owning the live palette every component renders
/// through (pi's module-global `theme` instance):
///
/// - Boot init: the constructor resolves the settings theme (slash values
///   read as unset) against the env-only COLORFGBG terminal theme and
///   initializes the palette with pi's silent dark fallback (pi ctor
///   `initTheme`).
/// - `apply_from_settings` is pi `applyFromSettings` minus the OSC 11/DSR
///   query, the automatic pair, and auto-sync: a defined setting applies
///   with `showError` (the verbatim `Failed to load theme "<name>":
///   <error>\nFell back to dark theme.`), an unset setting applies the
///   env detection and persists it to the global scope on high confidence.
/// - `apply_theme_name` replicates pi `applyThemeName`: failure replaces
///   the palette with `dark`, records the fallback as active, and reports
///   the verbatim message when `show_error` is set.
/// - `preview` is pi `preview`: in-memory apply without committing or
///   changing the active name; success invalidates and requests a render.
/// - Every change invalidates the borrowed root and fires `on_changed`
///   (pi `notifyChanged` → `ui.invalidate()` + `onChanged`), which the
///   interactive host wires to the editor border refresh.
///
/// OSC 11/DSR queries, the automatic `light/dark` pair, auto-sync, and the
/// file watcher are absent with no placeholder.
class ThemeController final {
public:
    /// `custom_themes_dir` is the user custom-themes directory
    /// (`<agent_config_directory>/themes`; empty skips the directory).
    /// `registered` are the resource-loader themes (pi `setRegisteredThemes`
    /// in the interactive-mode ctor). `theme_setting`/`committer` back the
    /// global-scope `theme` User Settings field.
    ThemeController(
        std::filesystem::path custom_themes_dir,
        std::vector<RegisteredTheme> registered,
        ThemeSettingProvider theme_setting,
        ThemeSettingCommitter committer,
        cch::tui::TerminalColorCapability color_capability,
        cch::tui::Tui& root,
        ThemeErrorSink show_error,
        ThemeChangedSink on_changed);
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

    /// pi `applyFromSettings` (sync subset; see the class comment).
    void apply_from_settings();

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// pi `initTheme` subset: the boot theme init for all modes before the
/// no-model guard (pi main.ts). Resolves the settings theme (slash values
/// read as unset) or the COLORFGBG-detected default, loads it (builtins,
/// then `<agent_config_directory>/themes/<name>.json`), and falls back to
/// `dark` silently on failure — the palette every mode boots with. The
/// interactive controller re-inits from the same settings at TUI
/// construction (pi `InteractiveThemeController`), and the startup-TUI host
/// (pi `startup-ui.ts` `createStartupTui`) consumes the same init.
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
