#pragma once

#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

namespace cch::tests {

/// Default bindings plus the frozen fullscreen viewport definitions, resolved
/// into a registry. Shared by fullscreen viewport and scroll-evidence tests.
[[nodiscard]] inline std::shared_ptr<const cch::tui::KeybindingRegistry> fullscreen_registry(
        std::vector<cch::tui::KeybindingOverride> overrides = {}) {
    auto definitions = cch::tui::builtin_tui_keybinding_definitions();
    auto fullscreen = cch::tui::alt_screen_keybinding_definitions();
    definitions.insert(
            definitions.end(), std::make_move_iterator(fullscreen.begin()), std::make_move_iterator(fullscreen.end()));
    auto resolution = cch::tui::resolve_keybindings(cch::tui::KeybindingResolutionRequest{
            .definitions = std::move(definitions), .overrides = std::move(overrides)});
    REQUIRE(resolution);
    return resolution->registry;
}

} // namespace cch::tests
