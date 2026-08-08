#include "coding_agent/tui/SettingsSelector.hpp"

#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/SettingsList.hpp>

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

/// The #327 subset items pi's settings selector renders, in pi's list order
/// (the render-settings group precedes the base items, so output-padding
/// renders before hide-thinking). The Theme item renders only when the
/// theme submenu factory is wired.
[[nodiscard]] std::vector<cch::tui::SettingItem> make_items(
    const SettingsSelectorConfig& config,
    bool theme_wired) {
    std::vector<cch::tui::SettingItem> items;
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
    if (theme_wired) {
        items.push_back({
            .id = "theme",
            .label = "Theme",
            .description = "Color theme for the interface",
            .current_value = config.current_theme,
            .control = cch::tui::SettingSubmenu{},
        });
    }
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
          list_(make_items(config, static_cast<bool>(state->callbacks.theme_submenu_factory)),
                make_options(theme, std::move(keybindings), config, state)) {}

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
                // `thinking` reports through the submenu's select sink and
                // `theme` commits inside the theme submenu, so both fire a
                // change here only to refresh the item's display value.
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
            if (item.id == "theme" && state->callbacks.theme_submenu_factory) {
                return state->callbacks.theme_submenu_factory(item, std::move(done));
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

util::Expected<cch::tui::RenderResult> SettingsSelectorComponent::render(
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
