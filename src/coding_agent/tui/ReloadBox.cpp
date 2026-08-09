#include "coding_agent/tui/ReloadBox.hpp"

#include "coding_agent/tui/DynamicBorder.hpp"

#include <cch/tui/Container.hpp>
#include <cch/tui/Text.hpp>

#include <string>
#include <utility>

namespace cch::coding_agent::tui {

std::shared_ptr<cch::tui::Component> make_reload_box(const LiveTheme& theme) {
    auto box = std::make_shared<cch::tui::Container>();
    // The hook is self-contained (captures the live theme's shared impl), so
    // the box outlives any single palette generation safely.
    auto border_color = theme.foreground_hook(ThemeToken::BorderAccent);
    (void)box->add_child(std::make_unique<DynamicBorder>(std::move(border_color)));
    (void)box->add_child(std::make_unique<cch::tui::Spacer>(1));
    (void)box->add_child(std::make_unique<cch::tui::Text>(
        theme.foreground(ThemeToken::Muted, std::string{kReloadBoxMessage}),
        1,
        0));
    (void)box->add_child(std::make_unique<cch::tui::Spacer>(1));
    // The border hook is move-only; a fresh hook styles the bottom rule.
    (void)box->add_child(std::make_unique<DynamicBorder>(
        theme.foreground_hook(ThemeToken::BorderAccent)));
    return box;
}

} // namespace cch::coding_agent::tui
