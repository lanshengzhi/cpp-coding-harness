#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

struct HotkeyHelpEntry {
    std::string id{};
    std::string keys{};
    std::string description{};
    std::string category{};
};

[[nodiscard]] std::vector<HotkeyHelpEntry> hotkey_help_entries(
    const cch::tui::KeybindingRegistry& registry);
[[nodiscard]] std::string key_hint(
    const cch::tui::KeybindingRegistry& registry,
    std::string_view action_id,
    std::string_view description);
[[nodiscard]] std::unique_ptr<cch::tui::Component> make_hotkey_help_view(
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry);

} // namespace cch::coding_agent::tui
