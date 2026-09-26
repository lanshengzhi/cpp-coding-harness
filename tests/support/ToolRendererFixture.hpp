#pragma once

#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/ToolExecutionComponent.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/Utils.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

/// The composed rows of one tool execution block: `raw` verbatim, `visible`
/// with the terminal sequences removed, the box's one-column margin dropped,
/// and the right padding trimmed. A tool-renderer case asserts `visible` so it
/// reads the text a user sees, and `raw` when the case is about the sequences
/// themselves. The margin is dropped rather than all leading spaces so a
/// renderer's own indentation stays assertable.
struct ToolRenderScreen {
    std::vector<std::string> raw{};
    std::vector<std::string> visible{};
};

/// The tool block's box padding (`ToolExecutionComponent`'s `Box(1, 1, …)`).
inline constexpr std::size_t kToolBlockMargin = 1;

[[nodiscard]] inline ToolRenderScreen render_tool_screen(
        coding_agent::tui::ToolExecutionComponent& component, std::size_t width) {
    const auto rendered = component.render(width);
    REQUIRE(rendered);
    ToolRenderScreen screen;
    screen.raw = rendered->lines;
    screen.visible.reserve(screen.raw.size());
    for (const auto& line : screen.raw) {
        auto visible = tui::strip_terminal_sequences(line);
        const auto first = visible.find_first_not_of(' ');
        if (first == std::string::npos) {
            screen.visible.emplace_back();
            continue;
        }
        const auto last = visible.find_last_not_of(' ');
        const auto start = std::min(first, kToolBlockMargin);
        screen.visible.push_back(visible.substr(start, last - start + 1));
    }
    return screen;
}

[[nodiscard]] inline coding_agent::tui::LiveTheme tool_render_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

/// The keybinding slot a running session resolves: the tui built-ins plus the
/// application table's `app.tools.expand`, which the tool fold hint reads.
/// `bound` false installs a registry with no entries at all, which is how a
/// session reaches the `Unbound` presentation of an action nothing is bound
/// to.
[[nodiscard]] inline std::shared_ptr<const coding_agent::tui::SharedKeybindings> tool_render_keybindings(
        bool bound = true) {
    if (!bound) {
        return std::make_shared<coding_agent::tui::SharedKeybindings>(
                std::make_shared<const tui::KeybindingRegistry>(std::vector<tui::EffectiveKeybinding>{}));
    }
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    constexpr std::array<std::string_view, 1> kExpand{"app.tools.expand"};
    const auto application = coding_agent::tui::app_keybinding_definitions(kExpand);
    REQUIRE(application);
    request.definitions.insert(request.definitions.end(), application->begin(), application->end());
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return std::make_shared<coding_agent::tui::SharedKeybindings>(resolved->registry);
}

} // namespace cch::tests
