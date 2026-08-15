#include "coding_agent/tui/SettingsSelector.hpp"

#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Container.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

// pi `settings-selector.ts` `THINKING_DESCRIPTIONS`, verbatim.
[[nodiscard]] std::string_view thinking_level_description(std::string_view level) {
    if (level == "off") return "No reasoning";
    if (level == "minimal") return "Very brief reasoning (~1k tokens)";
    if (level == "low") return "Light reasoning (~2k tokens)";
    if (level == "medium") return "Moderate reasoning (~8k tokens)";
    if (level == "high") return "Deep reasoning (~16k tokens)";
    if (level == "xhigh") return "Extra-high reasoning (~32k tokens)";
    if (level == "max") return "Maximum reasoning";
    return {};
}

// pi `settings-selector.ts` `DEFAULT_PROJECT_TRUST_LABELS`, verbatim.
[[nodiscard]] std::string_view default_project_trust_label(DefaultProjectTrust trust) {
    switch (trust) {
    case DefaultProjectTrust::Ask:
        return "Ask";
    case DefaultProjectTrust::Always:
        return "Always trust";
    case DefaultProjectTrust::Never:
        return "Never trust";
    }
    return "Ask";
}

[[nodiscard]] std::optional<DefaultProjectTrust> default_project_trust_by_label(
    std::string_view label) {
    if (label == "Ask") return DefaultProjectTrust::Ask;
    if (label == "Always trust") return DefaultProjectTrust::Always;
    if (label == "Never trust") return DefaultProjectTrust::Never;
    return std::nullopt;
}

struct SettingsSelectorState {
    SettingsSelectorCallbacks callbacks;
};

/// The thinking-level submenu (pi's `SelectSubmenu` for the `thinking` item):
/// one select list of the model's available levels with pi's per-level
/// descriptions, pre-selecting the current level. Selection reports the level
/// through the change sink and completes; cancel completes without a value.
[[nodiscard]] std::unique_ptr<cch::tui::SelectList> make_thinking_submenu(
    const LiveTheme& theme,
    const std::shared_ptr<const cch::tui::KeybindingRegistry>& keybindings,
    const SettingsSelectorConfig& config,
    SettingsSelectorState& state,
    cch::tui::SettingsSubmenuDoneSink done) {
    std::vector<cch::tui::SelectItem> items;
    items.reserve(config.available_thinking_levels.size());
    std::size_t selected_index = 0;
    for (std::size_t index = 0; index < config.available_thinking_levels.size(); ++index) {
        if (config.available_thinking_levels[index] == config.thinking_level) {
            selected_index = index;
        }
        items.push_back({
            .value = config.available_thinking_levels[index],
            .label = config.available_thinking_levels[index],
            .description = std::optional<std::string>{
                thinking_level_description(config.available_thinking_levels[index])},
        });
    }

    auto completion = std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
    auto list = std::make_unique<cch::tui::SelectList>(
        std::move(items),
        cch::tui::SelectListOptions{
            .max_visible = 10,
            .theme = theme.select_list_theme(),
            .on_select = [completion, state = &state](
                             const cch::tui::SelectItem& item) {
                // The state outlives the submenu (both live in the selector).
                if (state->callbacks.on_thinking_level_change) {
                    state->callbacks.on_thinking_level_change(item.value);
                }
                (*completion)(item.value);
            },
            .on_cancel = [completion]() { (*completion)(std::nullopt); },
            .keybindings = keybindings,
        });
    list->set_selected_index(selected_index);
    return list;
}

/// pi `settings-selector.ts` `ThemeSubmenu` single-mode subset: no Automatic
/// entry. Lists every available theme (pi `getAvailableThemes`, already
/// sorted) with the `(current)` marker on the active theme, pre-selecting
/// the active theme (falling back to `dark`, then the first item). Moving
/// the selection previews in memory (pi `onThemePreview` →
/// `themeController.preview`); confirming completes with the selected name
/// (the settings-list change sink then commits the global-scope settings
/// write and re-applies, pi `onThemeChange`); cancel re-previews the
/// original theme setting and completes without a value — the settings are
/// never written on cancel (cancel-does-not-revert).
class ThemeSubmenu final : public cch::tui::Component,
                           public cch::tui::InputHandler,
                           public cch::tui::Focusable {
public:
    ThemeSubmenu(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        const SettingsSelectorConfig& config,
        SettingsSelectorState& state,
        cch::tui::SettingsSubmenuDoneSink done) {
        const auto preview = [&state](std::string value) {
            if (state.callbacks.on_theme_preview) {
                state.callbacks.on_theme_preview(std::move(value));
            }
        };
        auto completion =
            std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
        const auto original_theme_setting = config.current_theme;

        // pi `SelectSubmenu` chrome: bold accent title, muted description
        // (trimmed — the subset has no Automatic entry), the select list,
        // and the dim hint line.
        auto content = std::make_unique<cch::tui::Container>();
        const auto styled = [&theme](ThemeToken token, std::string text) {
            return theme.foreground(token, std::move(text));
        };
        (void)content->add_child(std::make_unique<cch::tui::Text>(
            "\x1b[1m" + styled(ThemeToken::Accent, "Theme") + "\x1b[22m",
            /* padding_x */ 0,
            /* padding_y */ 0));
        (void)content->add_child(std::make_unique<cch::tui::Spacer>(1));
        (void)content->add_child(std::make_unique<cch::tui::Text>(
            styled(ThemeToken::Muted, "Select a theme."),
            /* padding_x */ 0,
            /* padding_y */ 0));
        (void)content->add_child(std::make_unique<cch::tui::Spacer>(1));

        std::vector<cch::tui::SelectItem> items;
        items.reserve(config.available_themes.size());
        std::size_t selected_index = 0;
        bool active_found = false;
        bool dark_found = false;
        for (std::size_t index = 0; index < config.available_themes.size(); ++index) {
            const auto& name = config.available_themes[index];
            if (name == config.active_theme) {
                selected_index = index;
                active_found = true;
            }
            if (name == "dark") dark_found = true;
            items.push_back({
                .value = name,
                .label = name,
                .description = name == config.active_theme
                    ? std::optional<std::string>{"(current)"}
                    : std::nullopt,
            });
        }
        if (!active_found) {
            // pi `preferredTheme(availableThemes, current, "dark")`: fall
            // back to `dark`, then the first item.
            const auto dark = std::find(
                config.available_themes.begin(),
                config.available_themes.end(),
                "dark");
            selected_index = dark_found
                ? static_cast<std::size_t>(dark - config.available_themes.begin())
                : 0;
        }

        auto select_list = std::make_unique<cch::tui::SelectList>(
            std::move(items),
            cch::tui::SelectListOptions{
                .max_visible = 10,
                .theme = theme.select_list_theme(),
                .on_select = [completion](const cch::tui::SelectItem& item) {
                    (*completion)(item.value);
                },
                .on_cancel = [completion, preview, original_theme_setting]() {
                    // pi `cancel()`: re-preview the original setting (never
                    // a settings write) and complete without a value.
                    preview(original_theme_setting);
                    (*completion)(std::nullopt);
                },
                .on_selection_change = [preview](const cch::tui::SelectItem& item) {
                    preview(item.value);
                },
                .keybindings = std::move(keybindings),
            });
        select_list_pointer_ = select_list.get();
        select_list->set_selected_index(selected_index);
        (void)content->add_child(std::move(select_list));
        (void)content->add_child(std::make_unique<cch::tui::Spacer>(1));
        (void)content->add_child(std::make_unique<cch::tui::Text>(
            styled(ThemeToken::Dim, "  Enter to select · Esc to go back"),
            /* padding_x */ 0,
            /* padding_y */ 0));
        content_ = std::move(content);
    }

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        return content_->render(width);
    }

    void invalidate() override {
        content_->invalidate();
    }

    void handle_input(const cch::tui::InputEventVariant& input) override {
        if (select_list_pointer_ != nullptr) {
            select_list_pointer_->handle_input(input);
        }
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return select_list_pointer_ != nullptr &&
            select_list_pointer_->accepts_key_releases();
    }

    void set_focused(bool focused) override {
        if (select_list_pointer_ != nullptr) {
            select_list_pointer_->set_focused(focused);
        }
    }

    [[nodiscard]] bool focused() const override {
        return select_list_pointer_ != nullptr && select_list_pointer_->focused();
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return select_list_pointer_ != nullptr
            ? select_list_pointer_->cursor_location()
            : std::nullopt;
    }

private:
    std::unique_ptr<cch::tui::Container> content_;
    // The SelectList is owned by the container; the pointer keeps input
    // routing and focus on it.
    cch::tui::SelectList* select_list_pointer_{nullptr};
};

/// The #327 subset items pi's settings selector renders, in pi's list order
/// (the render-settings group precedes the base items, so output-padding
/// renders before hide-thinking). The Theme item renders always (pi's
/// settings-selector.ts renders it unconditionally).
[[nodiscard]] std::vector<cch::tui::SettingItem> make_items(
    const SettingsSelectorConfig& config) {
    std::vector<cch::tui::SettingItem> items;
    items.push_back({
        .id = "skill-commands",
        .label = "Skill commands",
        .description = "Register skills as /skill:name commands",
        .current_value = config.enable_skill_commands ? "true" : "false",
        .control = cch::tui::SettingValues{{"true", "false"}},
    });
    items.push_back({
        .id = "output-padding",
        .label = "Output padding",
        .description = "Horizontal padding for user messages, assistant messages, and thinking",
        .current_value = std::to_string(config.output_pad),
        .control = cch::tui::SettingValues{{"0", "1"}},
    });
    items.push_back({
        .id = "hide-thinking",
        .label = "Hide thinking",
        .description = "Hide thinking blocks in assistant responses",
        .current_value = config.hide_thinking_block ? "true" : "false",
        .control = cch::tui::SettingValues{{"true", "false"}},
    });
    items.push_back({
        .id = "default-project-trust",
        .label = "Default project trust",
        .description = "Fallback behavior when no extension or saved trust decision decides project trust",
        .current_value = std::string{default_project_trust_label(config.default_project_trust)},
        .control = cch::tui::SettingValues{
            {"Ask", "Always trust", "Never trust"}},
    });
    items.push_back({
        .id = "thinking",
        .label = "Thinking level",
        .description = "Reasoning depth for thinking-capable models",
        .current_value = config.thinking_level,
        .control = cch::tui::SettingSubmenu{},
    });
    items.push_back({
        .id = "theme",
        .label = "Theme",
        .description = "Color theme for the interface",
        .current_value = config.current_theme,
        .control = cch::tui::SettingSubmenu{},
    });
    return items;
}

} // namespace

struct SettingsSelectorComponent::Impl {
    Impl(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        SettingsSelectorConfig config,
        SettingsSelectorCallbacks callbacks)
        : state(std::make_shared<SettingsSelectorState>(
              SettingsSelectorState{.callbacks = std::move(callbacks)})),
          list_(make_items(config), make_options(theme, std::move(keybindings), config, state)) {}

    [[nodiscard]] static cch::tui::SettingsListOptions make_options(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        const SettingsSelectorConfig& config,
        const std::shared_ptr<SettingsSelectorState>& state) {
        auto cancel = std::make_shared<SettingsSelectorCancelSink>(
            std::move(state->callbacks.on_cancel));
        auto submenu_keybindings = keybindings;
        auto options = cch::tui::SettingsListOptions{
            .max_visible = 10,
            .enable_search = true,
            .theme = theme.settings_list_theme(),
            .on_change = [state](std::string id, std::string new_value) {
                if (id == "skill-commands") {
                    if (state->callbacks.on_enable_skill_commands_change) {
                        state->callbacks.on_enable_skill_commands_change(new_value == "true");
                    }
                    return;
                }
                if (id == "hide-thinking") {
                    if (state->callbacks.on_hide_thinking_block_change) {
                        state->callbacks.on_hide_thinking_block_change(new_value == "true");
                    }
                    return;
                }
                if (id == "output-padding") {
                    if (state->callbacks.on_output_pad_change) {
                        state->callbacks.on_output_pad_change(new_value == "0" ? 0 : 1);
                    }
                    return;
                }
                if (id == "default-project-trust") {
                    const auto trust = default_project_trust_by_label(new_value);
                    if (trust && state->callbacks.on_default_project_trust_change) {
                        state->callbacks.on_default_project_trust_change(*trust);
                    }
                    return;
                }
                if (id == "theme") {
                    // pi `case "theme": callbacks.onThemeChange(newValue)`
                    // — the ThemeSubmenu completes with the selected name
                    // and the settings-list change sink commits it.
                    if (state->callbacks.on_theme_change) {
                        state->callbacks.on_theme_change(std::move(new_value));
                    }
                    return;
                }
                // `thinking` reports through the submenu's select sink, so
                // it fires a change here only to refresh the item's display
                // value.
            },
            .on_cancel = [cancel]() {
                if (*cancel) (*cancel)();
            },
            .keybindings = std::move(keybindings),
        };
        options.submenu_factory = [&theme, submenu_keybindings, config, state](
                                      const cch::tui::SettingItem& item,
                                      cch::tui::SettingsSubmenuDoneSink done) {
            if (item.id == "thinking") {
                return std::unique_ptr<cch::tui::Component>{
                    make_thinking_submenu(
                        theme, submenu_keybindings, config, *state, std::move(done))};
            }
            if (item.id == "theme") {
                return std::unique_ptr<cch::tui::Component>{std::make_unique<ThemeSubmenu>(
                    theme, submenu_keybindings, config, *state, std::move(done))};
            }
            return std::unique_ptr<cch::tui::Component>{};
        };
        return options;
    }

    std::shared_ptr<SettingsSelectorState> state;
    cch::tui::SettingsList list_;
};

SettingsSelectorComponent::SettingsSelectorComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    SettingsSelectorConfig config,
    SettingsSelectorCallbacks callbacks)
    : impl_(std::make_unique<Impl>(
          theme,
          std::move(keybindings),
          std::move(config),
          std::move(callbacks))) {}

SettingsSelectorComponent::~SettingsSelectorComponent() = default;

support::Expected<cch::tui::RenderResult> SettingsSelectorComponent::render(
    std::size_t width) {
    return impl_->list_.render(width);
}

void SettingsSelectorComponent::invalidate() {
    impl_->list_.invalidate();
}

void SettingsSelectorComponent::handle_input(
    const cch::tui::InputEventVariant& input) {
    impl_->list_.handle_input(input);
}

bool SettingsSelectorComponent::accepts_key_releases() const {
    return impl_->list_.accepts_key_releases();
}

void SettingsSelectorComponent::set_focused(bool focused) {
    impl_->list_.set_focused(focused);
}

bool SettingsSelectorComponent::focused() const {
    return impl_->list_.focused();
}

std::optional<cch::tui::CursorPosition>
SettingsSelectorComponent::cursor_location() const {
    return impl_->list_.cursor_location();
}

} // namespace cch::coding_agent::tui
