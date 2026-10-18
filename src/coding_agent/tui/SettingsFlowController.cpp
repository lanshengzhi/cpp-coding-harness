#include "SettingsFlowController.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/SettingsSelector.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "coding_agent/tui/ThinkingSelector.hpp"

#include <cch/ai/Model.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

namespace {

/// pi `settings-selector.ts` renders the TUI mode as the `tuiMode` setting's own
/// wire value; the settings manager stores and returns it in that spelling.
[[nodiscard]] std::string tui_mode_value(coding_agent::TuiMode mode) {
    return mode == coding_agent::TuiMode::Regular ? "regular" : "fullscreen";
}

/// The inverse for `onTuiModeChange(newValue as TuiMode)`: one of the two values
/// the item offers, resolved back to the mode the engine switches on.
[[nodiscard]] coding_agent::TuiMode tui_mode_for_value(std::string_view value) {
    return value == "regular" ? coding_agent::TuiMode::Regular : coding_agent::TuiMode::Fullscreen;
}

} // namespace

SettingsFlowController::SettingsFlowController(boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        SettingsFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings,
        coding_agent::SettingsManager* settings_manager,
        ThemeController* theme_controller)
    : executor_(std::move(executor)), presenter_(&presenter), host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)), keybindings_(std::move(keybindings)), settings_manager_(settings_manager),
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
    if (session == nullptr || !session->is_open() || theme_controller_ == nullptr || keybindings_ == nullptr ||
            settings_manager_ == nullptr) {
        return;
    }
    if (hooks_.overlay_active && hooks_.overlay_active()) return;

    const auto snapshot = session->snapshot();
    SettingsSelectorConfig config;
    config.hide_thinking_block = hooks_.hide_thinking_block && hooks_.hide_thinking_block();
    config.output_pad = hooks_.output_pad ? hooks_.output_pad() : 0;
    config.enable_skill_commands = settings_manager_->get_enable_skill_commands();
    config.show_hardware_cursor = hooks_.show_hardware_cursor && hooks_.show_hardware_cursor();
    config.show_terminal_progress = settings_manager_->get_show_terminal_progress();
    // pi settings-selector.ts config: the fullscreen rows read the same typed
    // values the live consumers use, so the rows show the resolved state after
    // a surgical write or a reload.
    config.fullscreen_exit_output = settings_manager_->get_fullscreen_exit_output();
    config.fullscreen_scrollbar = settings_manager_->get_fullscreen_scrollbar();
    config.fullscreen_copy_on_select = settings_manager_->get_fullscreen_copy_on_select();
    config.fullscreen_wheel_scroll_lines = settings_manager_->get_fullscreen_wheel_scroll_lines();
    config.thinking_level = snapshot.agent_state.thinking_level;
    const auto supported = ai::get_supported_thinking_levels(snapshot.agent_state.model);
    config.available_thinking_levels.reserve(supported.size());
    for (const auto level : supported) {
        if (const auto name = ai::model_thinking_level_name(level)) {
            config.available_thinking_levels.emplace_back(*name);
        }
    }
    config.default_project_trust = settings_manager_->default_project_trust().value_or(DefaultProjectTrust::Ask);
    // pi settings-selector.ts config: the raw theme setting (`|| "dark"`),
    // the active theme name (the `(current)` marker source), and the
    // sorted available themes.
    config.current_theme = settings_manager_->global_settings().theme.value_or("dark");
    config.active_theme = std::string{theme_controller_->active_theme_name()};
    config.available_themes = theme_controller_->available_theme_names();
    config.current_tui_mode = tui_mode_value(settings_manager_->get_tui_mode());

    // The TUI-mode row rolls back to the live renderer's mode when a switch is
    // refused (pi `selector?.getSettingsList().updateValue("tui-mode",
    // this.ui.mode)`), and the selector is built from the callbacks below, so the
    // row is reached through a slot the callback can outlive.
    auto selector_slot = std::make_shared<std::weak_ptr<SettingsSelectorComponent>>();

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
                    if (auto saved = self->settings_manager_->set_enable_skill_commands(enabled); !saved) {
                        self->show_error(combined_error_text(saved.error()));
                    }
                }
                if (self->hooks_.rebuild_autocomplete_provider) {
                    self->hooks_.rebuild_autocomplete_provider();
                }
            });
        }
    };
    callbacks.on_show_hardware_cursor_change = [weak](bool enabled) {
        if (const auto self = weak.lock()) {
            self->post([self, enabled] { self->set_show_hardware_cursor_setting(enabled); });
        }
    };
    callbacks.on_show_terminal_progress_change = [weak](bool enabled) {
        if (const auto self = weak.lock()) {
            self->post([self, enabled] {
                if (self->settings_manager_ != nullptr) {
                    if (auto saved = self->settings_manager_->set_show_terminal_progress(enabled); !saved) {
                        self->show_error(combined_error_text(saved.error()));
                    }
                }
            });
        }
    };
    callbacks.on_fullscreen_exit_output_change = [weak](FullscreenExitOutput output) {
        if (const auto self = weak.lock()) {
            self->post([self, output] { self->set_fullscreen_exit_output_setting(output); });
        }
    };
    callbacks.on_fullscreen_scrollbar_change = [weak](FullscreenScrollbar mode) {
        if (const auto self = weak.lock()) {
            self->post([self, mode] { self->set_fullscreen_scrollbar_setting(mode); });
        }
    };
    callbacks.on_fullscreen_copy_on_select_change = [weak](bool enabled) {
        if (const auto self = weak.lock()) {
            self->post([self, enabled] { self->set_fullscreen_copy_on_select_setting(enabled); });
        }
    };
    callbacks.on_fullscreen_wheel_scroll_lines_change = [weak](FullscreenWheelScrollLines lines) {
        if (const auto self = weak.lock()) {
            self->post([self, lines] { self->set_fullscreen_wheel_scroll_lines_setting(lines); });
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
                if (auto saved = self->settings_manager_->set_default_project_trust(trust); !saved) {
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
                                coding_agent::SettingsScope::Global, theme_setting);
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
    callbacks.on_tui_mode_change = [weak, selector_slot](std::string value) {
        if (const auto self = weak.lock()) {
            self->post([self, value = std::move(value), selector_slot]() mutable {
                if (self->settings_manager_ == nullptr || !self->hooks_.switch_tui_mode) return;
                const auto requested = tui_mode_for_value(value);
                auto switched = self->hooks_.switch_tui_mode(requested);
                if (!switched || !*switched) {
                    // pi keeps the live mode selected: the refused row is put
                    // back before the status line explains why.
                    if (const auto selector = selector_slot->lock(); selector && self->hooks_.current_tui_mode) {
                        selector->update_value("tui-mode", tui_mode_value(self->hooks_.current_tui_mode()));
                    }
                    if (!switched) {
                        self->show_error(combined_error_text(switched.error()));
                    } else if (self->presenter_ != nullptr) {
                        self->presenter_->show_status("Close active overlays before changing TUI mode");
                    }
                    return;
                }
                // pi `settingsManager.setTuiMode(mode)` then `showStatus(`TUI
                // mode: ${mode}`)`; only the global scope is written, so the
                // next run starts on the mode this one is already rendering.
                if (auto saved = self->settings_manager_->set_tui_mode(requested); !saved) {
                    self->show_error(combined_error_text(saved.error()));
                }
                if (self->presenter_ != nullptr) self->presenter_->show_status("TUI mode: " + value);
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
            theme_controller_->live_theme(), keybindings_->get(), std::move(config), std::move(callbacks));
    *selector_slot = selector;
    if (presenter_ != nullptr) {
        presenter_->replace_prompt_slot(std::move(selector));
    }
}

void SettingsFlowController::set_hide_thinking_block_setting(bool hidden) {
    if (hooks_.set_hide_thinking_block) hooks_.set_hide_thinking_block(hidden);
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_hide_thinking_block(hidden); !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.rebuild_chat) hooks_.rebuild_chat();
}

void SettingsFlowController::set_output_pad_setting(std::size_t padding) {
    if (hooks_.set_output_pad) hooks_.set_output_pad(padding);
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_output_pad(padding); !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.rebuild_chat) hooks_.rebuild_chat();
}

void SettingsFlowController::set_show_hardware_cursor_setting(bool enabled) {
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_show_hardware_cursor(enabled); !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.set_show_hardware_cursor) hooks_.set_show_hardware_cursor(enabled);
}

void SettingsFlowController::set_fullscreen_exit_output_setting(FullscreenExitOutput output) {
    if (settings_manager_ == nullptr) return;
    if (auto persisted = settings_manager_->set_fullscreen_exit_output(output); !persisted) {
        show_error(combined_error_text(persisted.error()));
    }
}

void SettingsFlowController::set_fullscreen_scrollbar_setting(FullscreenScrollbar mode) {
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_fullscreen_scrollbar(mode); !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    // pi applies the change even when the write fails: the live renderer keeps
    // the user's choice while the status line reports the persistence error.
    if (hooks_.apply_fullscreen_scrollbar) hooks_.apply_fullscreen_scrollbar(mode);
}

void SettingsFlowController::set_fullscreen_copy_on_select_setting(bool enabled) {
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_fullscreen_copy_on_select(enabled); !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.apply_fullscreen_copy_on_select) hooks_.apply_fullscreen_copy_on_select(enabled);
}

void SettingsFlowController::set_fullscreen_wheel_scroll_lines_setting(FullscreenWheelScrollLines lines) {
    if (settings_manager_ != nullptr) {
        if (auto persisted = settings_manager_->set_fullscreen_wheel_scroll_lines(lines); !persisted) {
            show_error(combined_error_text(persisted.error()));
        }
    }
    if (hooks_.apply_fullscreen_wheel_scroll_lines) hooks_.apply_fullscreen_wheel_scroll_lines(lines);
}

void SettingsFlowController::toggle_thinking_block_visibility() {
    const bool hidden = hooks_.hide_thinking_block && !hooks_.hide_thinking_block();
    set_hide_thinking_block_setting(hidden);
    if (presenter_ != nullptr) {
        presenter_->show_status("Thinking blocks: " + std::string{hidden ? "hidden" : "visible"});
    }
}

void SettingsFlowController::apply_thinking_level(const std::string& level, bool persist) {
    auto* session = current_session();
    if (session == nullptr || presenter_ == nullptr) return;
    auto applied = session->set_thinking_level(level, ModelMutationOptions{.persist = persist});
    if (!applied) {
        show_error(combined_error_text(applied.error()));
        return;
    }
    presenter_->show_status(persist ? "Default thinking level: " + level : "Thinking level: " + level);
}

void SettingsFlowController::show_thinking_selector() {
    auto* session = current_session();
    if (session == nullptr || !session->is_open() || presenter_ == nullptr || keybindings_ == nullptr ||
            theme_controller_ == nullptr) {
        return;
    }
    if (hooks_.overlay_active && hooks_.overlay_active()) return;

    // pi showThinkingSelector: the available levels of the current model, the
    // current level marker, and the settings default for the ` · default`
    // marker (`getDefaultThinkingLevel() ?? DEFAULT_THINKING_LEVEL`).
    const auto snapshot = session->snapshot();
    std::vector<std::string> levels;
    const auto supported = ai::get_supported_thinking_levels(snapshot.agent_state.model);
    levels.reserve(supported.size());
    for (const auto level : supported) {
        if (const auto name = ai::model_thinking_level_name(level)) {
            levels.emplace_back(*name);
        }
    }
    const auto default_level =
            settings_manager_ != nullptr && settings_manager_->settings().default_thinking_level
                    ? std::optional<std::string>{*settings_manager_->settings().default_thinking_level}
                    : std::nullopt;

    const auto weak = weak_from_this();
    auto selector = std::make_shared<ThinkingSelectorComponent>(
            theme_controller_->live_theme(),
            keybindings_->get(),
            snapshot.agent_state.thinking_level,
            std::move(levels),
            [weak](std::string level) {
                if (const auto self = weak.lock()) {
                    self->post([self, level = std::move(level)]() mutable {
                        self->apply_thinking_level(level, false);
                        self->presenter_->restore_prompt_slot();
                    });
                }
            },
            [weak] {
                if (const auto self = weak.lock()) {
                    self->post([self] { self->presenter_->restore_prompt_slot(); });
                }
            },
            [weak](std::string level) {
                if (const auto self = weak.lock()) {
                    self->post([self, level = std::move(level)]() mutable {
                        self->apply_thinking_level(level, true);
                        self->presenter_->restore_prompt_slot();
                    });
                }
            },
            default_level);
    presenter_->replace_prompt_slot(std::move(selector));
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
