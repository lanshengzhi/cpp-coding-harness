#include "ExtensionUiHost.hpp"

#include <utility>

namespace cch::coding_agent::tui {

void ExtensionUiHost::bind(cch::tui::ITui& tui, ExtensionUiRegistrationFactory factory) {
    dispose();
    tui_ = &tui;
    factory_ = std::move(factory);
    refresh();
}

void ExtensionUiHost::refresh() {
    if (tui_ == nullptr || !factory_) return;
    reset();
    factory_(*this);
}

void ExtensionUiHost::reset() noexcept {
    listeners_.clear();
    editor_factory_ = nullptr;
}

void ExtensionUiHost::dispose() noexcept {
    reset();
    tui_ = nullptr;
    factory_ = nullptr;
}

void ExtensionUiHost::on_terminal_input(cch::tui::RawInputListener listener) {
    if (tui_ == nullptr || !listener) return;
    listeners_.push_back(tui_->add_raw_input_listener(std::move(listener)));
}

void ExtensionUiHost::set_editor_component(ExtensionEditorFactory factory) {
    // pi `ctx.ui.setEditorComponent`: the registration context records the
    // factory; the interactive engine installs it into the prompt slot right
    // after the registration context runs. Product install/restore reuses
    // this host rather than inventing a second one.
    editor_factory_ = std::move(factory);
}

std::shared_ptr<cch::tui::EditorComponent> ExtensionUiHost::make_editor(
        cch::tui::EditorTheme theme, std::shared_ptr<SharedKeybindings> keybindings) {
    if (tui_ == nullptr || !editor_factory_) return nullptr;
    return editor_factory_(*tui_, std::move(theme), std::move(keybindings));
}

} // namespace cch::coding_agent::tui
