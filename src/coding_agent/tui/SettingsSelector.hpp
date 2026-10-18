#pragma once

#include "coding_agent/tui/ThemeController.hpp"

#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `settings-selector.ts` config: the #327 settings field subset the C++
/// product consumes plus the two render settings graduated by the G2 record
/// (decision 10). Every value is already resolved (defaults applied).
struct SettingsSelectorConfig {
    /// Resolved pi `hideThinkingBlock` (default false).
    bool hide_thinking_block{false};
    /// Resolved pi `outputPad` (default 1; 0 or 1 only).
    std::size_t output_pad{1};
    /// Resolved pi `enableSkillCommands` (default true).
    bool enable_skill_commands{true};
    /// Resolved pi `showHardwareCursor` (default false).
    bool show_hardware_cursor{false};
    /// Resolved pi `terminal.showTerminalProgress` (default false).
    bool show_terminal_progress{false};
    /// The session's current thinking level (pi `session.thinkingLevel`).
    std::string thinking_level{};
    /// The thinking levels the active model supports (pi
    /// `session.getAvailableThinkingLevels()`).
    std::vector<std::string> available_thinking_levels{};
    /// The global-only default project trust (pi `getDefaultProjectTrust`).
    DefaultProjectTrust default_project_trust{DefaultProjectTrust::Ask};
    /// pi `settingsManager.getThemeSetting() || "dark"`: the raw global
    /// theme setting (slash automatic-pair values included) shown as the
    /// Theme item's value and re-previewed on submenu cancel.
    std::string current_theme{};
    /// The currently applied theme name (pi `theme-selector.ts` `(current)`
    /// marker source).
    std::string active_theme{};
    /// pi `getAvailableThemes()`: builtins + custom directory + registered
    /// themes, deduped and sorted by name.
    std::vector<std::string> available_themes{};
    /// The live `tuiMode` setting (`regular` / `fullscreen`), shown as the TUI
    /// mode item's value and the value a refused switch rolls back to.
    std::string current_tui_mode{"regular"};
    /// Resolved pi `fullscreenExitOutput` (default transcript), shown as the
    /// exit-output item's value; the exit path reads the manager itself.
    FullscreenExitOutput fullscreen_exit_output{FullscreenExitOutput::Transcript};
    /// Resolved pi `fullscreenScrollbar` (default auto; regular mode has no
    /// effect), shown as the scrollbar item's value.
    FullscreenScrollbar fullscreen_scrollbar{FullscreenScrollbar::Auto};
    /// Resolved pi `fullscreenCopyOnSelect` (default true), shown as the
    /// copy-on-select item's value.
    bool fullscreen_copy_on_select{true};
    /// Resolved pi `fullscreenWheelScrollLines` (default auto), shown as the
    /// wheel-scrolling item's value and the extra entry its cycle appends.
    FullscreenWheelScrollLines fullscreen_wheel_scroll_lines{AutoWheelScrollLines{}};
};

using SettingsSelectorHideThinkingSink = std::move_only_function<void(bool)>;
using SettingsSelectorOutputPadSink = std::move_only_function<void(std::size_t)>;
using SettingsSelectorEnableSkillCommandsSink = std::move_only_function<void(bool)>;
using SettingsSelectorShowHardwareCursorSink = std::move_only_function<void(bool)>;
using SettingsSelectorShowTerminalProgressSink = std::move_only_function<void(bool)>;
using SettingsSelectorThinkingLevelSink = std::move_only_function<void(std::string)>;
using SettingsSelectorDefaultProjectTrustSink = std::move_only_function<void(DefaultProjectTrust)>;
using SettingsSelectorThemeChangeSink = std::move_only_function<void(std::string)>;
using SettingsSelectorThemePreviewSink = std::move_only_function<void(std::string)>;
using SettingsSelectorTuiModeChangeSink = std::move_only_function<void(std::string)>;
using SettingsSelectorFullscreenExitOutputSink = std::move_only_function<void(FullscreenExitOutput)>;
using SettingsSelectorFullscreenScrollbarSink = std::move_only_function<void(FullscreenScrollbar)>;
using SettingsSelectorFullscreenCopyOnSelectSink = std::move_only_function<void(bool)>;
using SettingsSelectorFullscreenWheelScrollLinesSink = std::move_only_function<void(FullscreenWheelScrollLines)>;
using SettingsSelectorCancelSink = std::move_only_function<void()>;

/// pi `settings-selector.ts` callbacks: one sink per rendered subset item.
struct SettingsSelectorCallbacks {
    SettingsSelectorHideThinkingSink on_hide_thinking_block_change{};
    SettingsSelectorOutputPadSink on_output_pad_change{};
    SettingsSelectorEnableSkillCommandsSink on_enable_skill_commands_change{};
    SettingsSelectorShowHardwareCursorSink on_show_hardware_cursor_change{};
    SettingsSelectorShowTerminalProgressSink on_show_terminal_progress_change{};
    SettingsSelectorThinkingLevelSink on_thinking_level_change{};
    SettingsSelectorDefaultProjectTrustSink on_default_project_trust_change{};
    /// pi `onThemeChange`: commit a ThemeSubmenu selection (global-scope
    /// settings write) and re-apply the theme from settings.
    SettingsSelectorThemeChangeSink on_theme_change{};
    /// pi `onThemePreview`: in-memory preview of one theme name or setting.
    SettingsSelectorThemePreviewSink on_theme_preview{};
    /// pi `onTuiModeChange`: switch the live renderer and persist the mode on
    /// success; a refusal is reported and the row is rolled back by the caller.
    SettingsSelectorTuiModeChangeSink on_tui_mode_change{};
    /// pi `onFullscreenExitOutputChange`: persist the global-scope setting.
    SettingsSelectorFullscreenExitOutputSink on_fullscreen_exit_output_change{};
    /// pi `onFullscreenScrollbarChange`: persist and re-apply the transcript
    /// scrollbar mode.
    SettingsSelectorFullscreenScrollbarSink on_fullscreen_scrollbar_change{};
    /// pi `onFullscreenCopyOnSelectChange`: persist and re-apply the live
    /// fullscreen renderer's copy-on-select.
    SettingsSelectorFullscreenCopyOnSelectSink on_fullscreen_copy_on_select_change{};
    /// pi `onFullscreenWheelScrollLinesChange`: persist and re-apply the live
    /// fullscreen renderer's wheel line count.
    SettingsSelectorFullscreenWheelScrollLinesSink on_fullscreen_wheel_scroll_lines_change{};
    SettingsSelectorCancelSink on_cancel{};
};

/// The settings selector (pi `settings-selector.ts`): renders the #327
/// settings subset plus the graduated render settings (hideThinkingBlock,
/// outputPad) and the graduated enableSkillCommands toggle as a searchable
/// settings list in the editor slot. Value items cycle on confirm; the
/// thinking-level item opens a select submenu with pi's per-level
/// descriptions; the Theme item opens the single-mode ThemeSubmenu (pi's
/// `ThemeSubmenu` subset, no Automatic entry) with in-memory preview, a
/// global-scope settings commit on confirm, and cancel-does-not-revert.
///
/// Threading: constructed and driven on the TUI thread; callbacks fire on
/// the input thread like every selector sink, and callers post to their
/// executor for session/settings-touching work (pi's settings callbacks run
/// on the UI thread and write through the settings manager synchronously).
class SettingsSelectorComponent final : public cch::tui::Component,
                                        public cch::tui::InputHandler,
                                        public cch::tui::Focusable {
public:
    SettingsSelectorComponent(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            SettingsSelectorConfig config,
            SettingsSelectorCallbacks callbacks);
    SettingsSelectorComponent(SettingsSelectorComponent&&) = delete;
    SettingsSelectorComponent& operator=(SettingsSelectorComponent&&) = delete;
    ~SettingsSelectorComponent() override;
    SettingsSelectorComponent(const SettingsSelectorComponent&) = delete;
    SettingsSelectorComponent& operator=(const SettingsSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

    /// pi `selector?.getSettingsList().updateValue(id, value)`: replace one
    /// item's displayed value without running its change callback, so a refused
    /// TUI-mode switch can put the row back on the live mode.
    void update_value(std::string id, std::string new_value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
