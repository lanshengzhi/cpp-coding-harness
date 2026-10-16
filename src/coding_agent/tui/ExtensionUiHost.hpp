#pragma once

#include "coding_agent/tui/SharedKeybindings.hpp"

#include <cch/tui/Editor.hpp>
#include <cch/tui/ITui.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace cch::coding_agent::tui {

class ExtensionUiHost;

/// Linked C++ registration callback for one interactive extension UI context.
using ExtensionUiRegistrationFactory = std::move_only_function<void(ExtensionUiHost&)>;

/// Factory that builds one replaceable prompt editor (pi `EditorFactory`).
/// The theme arrives by value because its style hooks are move-only: every
/// factory call receives a fresh theme instance, matching pi's
/// `getEditorTheme()` object literal.
using ExtensionEditorFactory = std::move_only_function<std::shared_ptr<cch::tui::EditorComponent>(
        cch::tui::ITui& tui, cch::tui::EditorTheme theme, std::shared_ptr<SharedKeybindings> keybindings)>;

/// Frontend owner of extension registrations against the live TUI input chain.
/// The bound TUI must outlive the host's binding; registrations are disposed
/// synchronously before the TUI is unbound or stopped.
class ExtensionUiHost final {
public:
    ExtensionUiHost() = default;
    ExtensionUiHost(ExtensionUiHost&&) noexcept = delete;
    ExtensionUiHost& operator=(ExtensionUiHost&&) noexcept = delete;
    ExtensionUiHost(const ExtensionUiHost&) = delete;
    ExtensionUiHost& operator=(const ExtensionUiHost&) = delete;

    /// Bind the live TUI and install the first registration context.
    void bind(cch::tui::ITui& tui, ExtensionUiRegistrationFactory factory);

    /// Rebind the existing listener callbacks to a new root without invoking
    /// the extension registration factory again.
    void rebind(cch::tui::ITui& tui);

    /// Dispose current listener handles but keep the host ready to refresh.
    void reset() noexcept;

    /// Dispose registrations and release the TUI and factory references.
    void dispose() noexcept;

    /// Register one ordered terminal-input listener owned by this context.
    void on_terminal_input(cch::tui::RawInputListener listener);

    /// Install or clear the active custom prompt editor factory (pi
    /// `ctx.ui.setEditorComponent`). Pass an empty factory to restore the
    /// default editor. Product install/restore is owned by the interactive
    /// engine and must reuse this host rather than add a second one.
    void set_editor_component(ExtensionEditorFactory factory);

    /// Whether the current registration context installed a custom editor
    /// (pi `getEditorComponent() !== undefined`). Cleared by `reset()`, the
    /// pi `resetExtensionUI` restore point.
    [[nodiscard]] bool has_editor_component() const noexcept { return editor_factory_ != nullptr; }

    /// Build the registered custom editor (pi `factory(tui, theme,
    /// keybindings)`). The caller supplies the live editor theme and the
    /// shared keybindings the factory constructs with; null when the current
    /// context registered none.
    [[nodiscard]] std::shared_ptr<cch::tui::EditorComponent> make_editor(
            cch::tui::EditorTheme theme, std::shared_ptr<SharedKeybindings> keybindings);

private:
    cch::tui::ITui* tui_{nullptr}; // must outlive this host binding.
    ExtensionUiRegistrationFactory factory_{nullptr};
    ExtensionEditorFactory editor_factory_{nullptr};
    std::vector<cch::tui::RawInputListener> registered_listeners_;
    std::vector<cch::tui::RawInputListenerHandle> listeners_;
};

} // namespace cch::coding_agent::tui
