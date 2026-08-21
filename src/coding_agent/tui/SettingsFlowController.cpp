#include "SettingsFlowController.hpp"

#include "ai/ModelThinkingLevel.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/SettingsSelector.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/ThemeController.hpp"

#include <cch/ai/Model.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {

SettingsFlowController::SettingsFlowController(
    boost::asio::any_io_executor executor,
    ModalPresenter& presenter,
    std::weak_ptr<void> host_lifetime,
    SettingsFlowHostHooks hooks,
    std::shared_ptr<SharedKeybindings> keybindings,
    coding_agent::SettingsManager* settings_manager,
    ThemeController* theme_controller)
    : executor_(std::move(executor)),
      presenter_(&presenter),
      host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)),
      keybindings_(std::move(keybindings)),
      settings_manager_(settings_manager),
      theme_controller_(theme_controller) {}

void SettingsFlowController::post(std::move_only_function<void()> action) {
    if (hooks_.post_on_executor) {
        hooks_.post_on_executor(std::move(action));
    }
}

AgentSession* SettingsFlowController::current_session() {
    if (hooks_.current_session == nullptr) return nullptr;
    return hooks_.current_session();
}

void SettingsFlowController::show_error(const std::string& text) {
    if (presenter_ != nullptr) {
        presenter_->show_error(std::string{text});
    }
}

void SettingsFlowController::show_settings_selector() {
    if (hooks_.is_live == nullptr || !hooks_.is_live()) return;
    auto* session = current_session();
    if (session == nullptr || !session->is_open() || theme_controller_ == nullptr ||
        keybindings_ == nullptr || settings_manager_ == nullptr) {
        return;
    }
    if (hooks_.overlay_active && hooks_.overlay_active()) return;

    const auto snapshot = session->snapshot();
    SettingsSelectorConfig config;
    config.hide_thinking_block =
        hooks_.hide_thinking_block && hooks_.hide_thinking_block();
    config.output_pad = hooks_.output_pad ? hooks_.output_pad() : 0;
    config.enable_skill_commands =
        settings_manager_->get_enable_skill_commands();
    config.thinking_level = snapshot.agent_state.thinking_level;
    const auto supported = ai::get_supported_thinking_levels(snapshot.agent_state.model);
    config.available_thinking_levels.reserve(supported.size());
    for (const auto level : supported) {
        if (const auto name = ai::detail::model_thinking_level_name(level)) {
            config.available_thinking_levels.emplace_back(*name);
        }
    }
    config.default_project_trust =
        settings_manager_->default_project_trust().value_or(DefaultProjectTrust::Ask);
    // pi settings-selector.ts config: the raw theme setting (`|| "dark"`),
    // the active theme name (the `(current)` marker source), and the
    // sorted available themes.
    config.current_theme = settings_manager_->global_settings().theme.value_or("dark");
    config.active_theme = std::string{theme_controller_->active_theme_name()};
    config.available_themes = theme_controller_->available_theme_names();

    const auto weak = weak_from_this();
    SettingsSelectorCallbacks callbacks;
    callbacks.on_hide_thinking_block_change = [weak](bool hidden) {
        if (const auto self = weak.lock()) {
            self->post([self, hidden] { self->set_hide_thinking_block_setting(hidden); });
        }
    };
    callbacks.on_output_pad_change = [weak](std::size_t padding) {
        if (const auto self = weak.lock()) {
            self->post([self, padding] { self->set_output_pad_setting(padding); });
        }
    };
    callbacks.on_enable_skill_commands_change = [weak](bool enabled) {
        if (const auto self = weak.lock()) {
            self->post([self, enabled] {
                // pi `onEnableSkillCommandsChange`:
                // `setEnableSkillCommands(enabled)` then
                // `setupAutocompleteProvider()`.
                if (self->settings_manager_ != nullptr) {
                    if (auto saved =
                            self->settings_manager_->set_enable_skill_commands(enabled);
                        !saved) {
                        self->show_error(combined_error_text(saved.error()));
                    }
                }
                if (self->hooks_.rebuild_autocomplete_provider) {
                    self->hooks_.rebuild_autocomplete_provider();
                }
            });
        }
    };
    callbacks.on_thinking_level_change = [weak](std::string level) {
        if (const auto self = weak.lock()) {
            self->post([self, level = std::move(level)]() mutable {
                // pi `onThinkingLevelChange` → `session.setThinkingLevel`:
                // the session persists the `thinking_level_change` entry
                // and the global settings default itself.
                auto* session = self->current_session();
                if (session == nullptr) return;
                auto applied = session->set_thinking_level(level);
                if (!applied) {
                    self->show_error(combined_error_text(applied.error()));
                }
            });
        }
    };
    callbacks.on_default_project_trust_change = [weak](DefaultProjectTrust trust) {
        if (const auto self = weak.lock()) {
            self->post([self, trust] {
                if (self->settings_manager_ == nullptr) return;
                if (auto saved =
                        self->settings_manager_->set_default_project_trust(trust);
                    !saved) {
                    self->show_error(combined_error_text(saved.error()));
                }
            });
        }
    };
    callbacks.on_cancel = [weak] {
        if (const auto self = weak.lock()) {
            self->post([self] {
                if (self->presenter_ != nullptr) self->presenter_->restore_prompt_slot();
            });
        }
    };
    callbacks.on_theme_change = [weak](std::string theme_setting) {
        if (const auto self = weak.lock()) {
            self->post([self, theme_setting = std::move(theme_setting)]() mutable {
                // pi `onThemeChange`: `settingsManager.setTheme(themeSetting)`
                // then `themeController.applyFromSettings()`.
                if (self->settings_manager_ != nullptr) {
                    if (auto saved = self->settings_manager_->set_theme(
                            coding_agent::SettingsScope::Global,
                            theme_setting);
                        !saved) {
                        self->show_error(combined_error_text(saved.error()));
                    }
                }
                if (self->theme_controller_ != nullptr) {
                    self->theme_controller_->apply_from_settings();
                }
            });
        }
    };
    callbacks.on_theme_preview = [weak](std::string theme_name) {
        if (const auto self = weak.lock()) {
            self->post([self, theme_name = std::move(theme_name)]() mutable {
                if (self->theme_controller_ != nullptr) {
                    self->theme_controller_->preview(theme_name);
                }
            });
        }
    };

    auto selector = std::make_shared<SettingsSelectorComponent>(
        theme_controller_->live_theme(),
        keybindings_->get(),
        std::move(config),
        std::move(callbacks));
    if (presenter_ != nullptr) {
        presenter_->replace_prompt_slot(std::move(selector));
    }
}

void SettingsFlowController::set_hide_thinking_block_setting(bool hidden) {
    if (hooks_.set_hide_thinking_block) hooks_.set_hide_thinking_block(hidden);
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_hide_thinking_block(hidden);
            !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.rebuild_chat) hooks_.rebuild_chat();
}

void SettingsFlowController::set_output_pad_setting(std::size_t padding) {
    if (hooks_.set_output_pad) hooks_.set_output_pad(padding);
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_output_pad(padding);
            !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.rebuild_chat) hooks_.rebuild_chat();
}

void SettingsFlowController::toggle_thinking_block_visibility() {
    const bool hidden = hooks_.hide_thinking_block && !hooks_.hide_thinking_block();
    set_hide_thinking_block_setting(hidden);
    if (presenter_ != nullptr) {
        presenter_->show_status(
            "Thinking blocks: " + std::string{hidden ? "hidden" : "visible"});
    }
}

void SettingsFlowController::cycle_thinking_level() {
    auto* session = current_session();
    if (session == nullptr || presenter_ == nullptr) return;
    auto level = session->cycle_thinking_level();
    if (!level) {
        presenter_->show_error(combined_error_text(level.error()));
        return;
    }
    if (!*level) {
        presenter_->show_status("Current model does not support thinking");
        return;
    }
    presenter_->show_status("Thinking level: " + **level);
}

} // namespace cch::coding_agent::tui
