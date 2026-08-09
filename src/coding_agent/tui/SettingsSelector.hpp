#pragma once

#include "coding_agent/tui/ThemeController.hpp"

#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/util/Error.hpp>

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
};

using SettingsSelectorHideThinkingSink = std::move_only_function<void(bool)>;
using SettingsSelectorOutputPadSink = std::move_only_function<void(std::size_t)>;
using SettingsSelectorEnableSkillCommandsSink = std::move_only_function<void(bool)>;
using SettingsSelectorThinkingLevelSink = std::move_only_function<void(std::string)>;
using SettingsSelectorDefaultProjectTrustSink =
    std::move_only_function<void(DefaultProjectTrust)>;
using SettingsSelectorThemeChangeSink = std::move_only_function<void(std::string)>;
using SettingsSelectorThemePreviewSink = std::move_only_function<void(std::string)>;
using SettingsSelectorCancelSink = std::move_only_function<void()>;

/// pi `settings-selector.ts` callbacks: one sink per rendered subset item.
struct SettingsSelectorCallbacks {
    SettingsSelectorHideThinkingSink on_hide_thinking_block_change{};
    SettingsSelectorOutputPadSink on_output_pad_change{};
    SettingsSelectorEnableSkillCommandsSink on_enable_skill_commands_change{};
    SettingsSelectorThinkingLevelSink on_thinking_level_change{};
    SettingsSelectorDefaultProjectTrustSink on_default_project_trust_change{};
    /// pi `onThemeChange`: commit a ThemeSubmenu selection (global-scope
    /// settings write) and re-apply the theme from settings.
    SettingsSelectorThemeChangeSink on_theme_change{};
    /// pi `onThemePreview`: in-memory preview of one theme name or setting.
    SettingsSelectorThemePreviewSink on_theme_preview{};
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
class SettingsSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    SettingsSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        SettingsSelectorConfig config,
        SettingsSelectorCallbacks callbacks);
    SettingsSelectorComponent(SettingsSelectorComponent&&) = delete;
    SettingsSelectorComponent& operator=(SettingsSelectorComponent&&) = delete;
    ~SettingsSelectorComponent() override;
    SettingsSelectorComponent(const SettingsSelectorComponent&) = delete;
    SettingsSelectorComponent& operator=(const SettingsSelectorComponent&) = delete;

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
