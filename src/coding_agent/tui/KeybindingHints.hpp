#pragma once

#include "coding_agent/tui/SharedKeybindings.hpp"

#include <cch/tui/Component.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

class LiveTheme;

/// Formats one key text like pi `keybinding-hints.ts` `formatKeyText`:
/// slash-separated alternatives with plus-separated parts, optionally
/// capitalized ("alt+enter" -> "Alt+Enter").
[[nodiscard]] std::string format_key_text(
    std::string_view key,
    bool capitalize = false);

/// pi `keyHint`: dim key text followed by a muted description.
[[nodiscard]] std::string key_hint(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings,
    std::string_view action,
    std::string_view description);

/// pi `rawKeyHint`: same shape for literal keys.
[[nodiscard]] std::string raw_key_hint(
    const LiveTheme& theme,
    std::string_view key,
    std::string_view description);

/// The startup header: pi's built-in header with keybinding hints only (no
/// logo, G2). Renders the compact instruction line plus the `Press <key> to
/// show full startup help and loaded resources.` notice by default and the
/// full assembled-action instruction list when tools are expanded (pi
/// `interactive-mode.ts` builtInHeader expansion minus the logo and the
/// onboarding line). Keybindings resolve through the shared slot so a
/// `/reload` re-catalog reflects live (ADR 0035, #418).
class KeybindingHints final : public cch::tui::Component {
public:
    KeybindingHints(
        const LiveTheme& theme,
        std::shared_ptr<const SharedKeybindings> keybindings,
        bool user_bash_available,
        bool clipboard_paste_available);

    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    const LiveTheme& theme_; // must outlive this component.
    /// The shared keybinding slot (ADR 0035); the strong reference keeps the
    /// registry alive for every render.
    std::shared_ptr<const SharedKeybindings> keybindings_;
    bool user_bash_available_{false};
    bool clipboard_paste_available_{false};
    bool expanded_{false};
};

} // namespace cch::coding_agent::tui
