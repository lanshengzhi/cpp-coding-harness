#pragma once

// Repository-private composition host for the fullscreen TUI root (#1006).
// The toolkit owns selection geometry and lifecycle; this frontend host adapts
// selection copies, OSC 8 link opens and right-click paste to closed action seams.

#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"

#include <cch/tui/AltScreen.hpp>

#include <cstddef>
#include <functional>
#include <string_view>

namespace cch::coding_agent::tui {

/// Receives the outcome of automatic and explicit selection-copy requests.
/// Product status and error presentation stay at the frontend boundary.
using SelectionCopyHook = std::move_only_function<void(const ClipboardCopyResult&)>;

/// Reusable owner of an AltScreen root with a frontend clipboard action adapter.
/// The action sink and generation are borrowed from the composition host and
/// must outlive this object.
class AltScreenHost final {
public:
    AltScreenHost(cch::tui::Terminal& terminal,
            TuiActionSink& action_sink,
            const std::size_t& action_generation,
            SelectionCopyHook feedback = nullptr);
    ~AltScreenHost() = default;

    AltScreenHost(const AltScreenHost&) = delete;
    AltScreenHost& operator=(const AltScreenHost&) = delete;

    [[nodiscard]] cch::tui::AltScreen& alt_screen() noexcept { return alt_screen_; }
    [[nodiscard]] cch::tui::ITui& tui() noexcept { return alt_screen_; }

    void set_copy_on_select(bool enabled);
    [[nodiscard]] bool copy_on_select() const;
    [[nodiscard]] bool has_active_selection() const;
    [[nodiscard]] support::Expected<bool> copy_active_selection_to_clipboard();
    /// Handle pi's keyboard preference rule. Returns true when a selection
    /// copy was attempted, including a failed clipboard action, so callers do
    /// not also copy the last assistant message.
    [[nodiscard]] bool copy_selection_for_keyboard();

private:
    [[nodiscard]] support::ExpectedVoid copy_selection(std::string_view text);

    cch::tui::AltScreen alt_screen_;
    TuiActionSink& action_sink_;           // borrowed; must outlive this host.
    const std::size_t& action_generation_; // borrowed; must outlive this host.
    SelectionCopyHook feedback_;
};

} // namespace cch::coding_agent::tui
