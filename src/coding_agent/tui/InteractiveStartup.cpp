#include "InteractiveStartup.hpp"

#include "coding_agent/tui/ThemeController.hpp"

#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

std::vector<std::string> assemble_keybinding_actions(bool clipboard_paste_available) {
    std::vector<std::string> actions{
        "app.interrupt",
        "app.clear",
        "app.exit",
        // pi's main-editor `app.suspend` (Ctrl+Z) and
        // `app.editor.external` (Ctrl+G).
        "app.suspend",
        "app.editor.external",
        "app.tools.expand",
        "app.thinking.toggle",
        "app.thinking.cycle",
        "app.model.cycleForward",
        "app.model.cycleBackward",
        "app.model.select",
        "app.message.followUp",
        "app.message.dequeue",
        // pi `app.session.*`: recognized-but-unbound in the main editor
        // (defaultKeys [] — a user-assigned keybinding triggers the
        // flow) and selector-scoped inside the session selector.
        "app.session.new",
        "app.session.tree",
        "app.session.fork",
        "app.session.resume",
        "app.session.toggleSort",
        "app.session.toggleNamedFilter",
        "app.session.togglePath",
        "app.session.rename",
        "app.session.delete",
        "app.session.deleteNoninvasive",
        // pi's main-editor `app.message.copy` (assembled with the tree
        // selector, which matches the same action through the shared
        // registry; the copy flows land with P14's clipboard writer).
        "app.message.copy",
        // Selector-scoped: the tree selector matches the eleven
        // `app.tree.*` actions through the same registry (pi's shared
        // KeybindingsManager), with the main editor leaving them
        // unbound.
        "app.tree.foldOrUp",
        "app.tree.unfoldOrDown",
        "app.tree.editLabel",
        "app.tree.toggleLabelTimestamp",
        "app.tree.filter.default",
        "app.tree.filter.noTools",
        "app.tree.filter.userOnly",
        "app.tree.filter.labeledOnly",
        "app.tree.filter.all",
        "app.tree.filter.cycleForward",
        "app.tree.filter.cycleBackward",
        // Selector-scoped: the scoped-models selector matches the six
        // `app.models.*` actions through the same registry (pi's shared
        // KeybindingsManager).
        "app.models.save",
        "app.models.enableAll",
        "app.models.clearAll",
        "app.models.toggleProvider",
        "app.models.reorderUp",
        "app.models.reorderDown",
    };
    if (clipboard_paste_available) actions.push_back("app.clipboard.pasteImage");
    return actions;
}

support::Expected<KeybindingsManagerResult> load_app_keybinding_manager(
    const std::filesystem::path& agent_config_directory,
    const std::vector<std::string>& actions) {
    std::vector<std::string_view> action_views;
    action_views.reserve(actions.size());
    for (const auto& action : actions) {
        action_views.push_back(action);
    }
    if (auto definitions = app_keybinding_definitions(action_views); !definitions) {
        return std::unexpected(definitions.error());
    } else {
        KeybindingsManagerRequest request;
        request.agent_config_directory = agent_config_directory;
        request.application_definitions = std::move(*definitions);
        return load_keybindings_manager(std::move(request));
    }
}

support::Expected<coding_agent::SettingsManager> create_interactive_settings_manager(
    const std::filesystem::path& agent_config_directory) {
    auto manager = coding_agent::SettingsManager::create(
        /* cwd */ {},
        agent_config_directory,
        /* project_trusted */ false);
    for (const auto& settings_error : manager.errors()) {
        if (settings_error.scope == coding_agent::SettingsScope::Global) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "could not load global settings",
                settings_error.message));
        }
    }
    return manager;
}

std::unique_ptr<ThemeController> make_interactive_theme_controller(
    const std::filesystem::path& agent_config_directory,
    coding_agent::SettingsManager& settings_manager,
    cch::tui::TerminalColorCapability color_capability,
    cch::tui::Tui& root,
    InteractiveThemeHooks hooks) {
    return std::make_unique<ThemeController>(
        agent_config_directory.empty()
            ? std::filesystem::path{}
            : agent_config_directory / "themes",
        /* registered */ std::vector<RegisteredTheme>{},
        [manager = &settings_manager]() {
            return manager->global_settings().theme;
        },
        [manager = &settings_manager](std::string_view name) {
            return manager->set_theme(coding_agent::SettingsScope::Global, name);
        },
        color_capability,
        root,
        std::move(hooks.show_error),
        std::move(hooks.on_changed));
}

} // namespace cch::coding_agent::tui
