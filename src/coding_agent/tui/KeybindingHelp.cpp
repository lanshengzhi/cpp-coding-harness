#include "KeybindingHelp.hpp"

#include <cch/tui/Text.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

std::vector<HotkeyHelpEntry> hotkey_help_entries(
    const cch::tui::KeybindingRegistry& registry) {
    std::vector<HotkeyHelpEntry> result;
    result.reserve(registry.entries().size());
    for (const auto& entry : registry.entries()) {
        const auto keys = registry.key_text(entry.id);
        result.push_back({
            .id = entry.id,
            .keys = entry.available ? (keys.empty() ? "Unbound" : keys) : keys,
            .description = entry.description,
            .category = entry.category,
        });
    }
    return result;
}

std::string key_hint(
    const cch::tui::KeybindingRegistry& registry,
    std::string_view action_id,
    std::string_view description) {
    const auto keys = registry.key_text(action_id);
    return std::format("{} {}", keys.empty() ? "Unbound" : keys, description);
}

std::unique_ptr<cch::tui::Component> make_hotkey_help_view(
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry) {
    std::string text = "Hotkeys\n";
    if (registry) {
        auto entries = hotkey_help_entries(*registry);
        std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            const auto left_application = left.category == "Application";
            const auto right_application = right.category == "Application";
            if (left_application != right_application) return left_application;
            if (left.category != right.category) return left.category < right.category;
            return left.id < right.id;
        });
        std::string category;
        for (const auto& entry : entries) {
            if (entry.category != category) {
                category = entry.category;
                text += "\n" + category + "\n";
            }
            text += std::format("{}  {} — {}\n", entry.keys, entry.id, entry.description);
        }
    }
    return std::make_unique<cch::tui::Text>(std::move(text));
}

} // namespace cch::coding_agent::tui
