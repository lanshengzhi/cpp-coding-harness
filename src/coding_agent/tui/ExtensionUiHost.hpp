#pragma once

#include <cch/tui/ITui.hpp>

#include <functional>
#include <vector>

namespace cch::coding_agent::tui {

class ExtensionUiHost;

/// Linked C++ registration callback for one interactive extension UI context.
using ExtensionUiRegistrationFactory = std::move_only_function<void(ExtensionUiHost&)>;

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

    /// Dispose the current context and install a fresh one on the same TUI.
    void refresh();

    /// Dispose current listener handles but keep the host ready to refresh.
    void reset() noexcept;

    /// Dispose registrations and release the TUI and factory references.
    void dispose() noexcept;

    /// Register one ordered terminal-input listener owned by this context.
    void on_terminal_input(cch::tui::RawInputListener listener);

private:
    cch::tui::ITui* tui_{nullptr}; // must outlive this host binding.
    ExtensionUiRegistrationFactory factory_{nullptr};
    std::vector<cch::tui::RawInputListenerHandle> listeners_;
};

} // namespace cch::coding_agent::tui
