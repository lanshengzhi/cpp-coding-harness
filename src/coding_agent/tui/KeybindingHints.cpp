#include "KeybindingHints.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include "coding_agent/tui/Theme.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string capitalize_part(std::string_view part) {
    if (part.empty()) return std::string{part};
    std::string result{part};
    result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

[[nodiscard]] std::string styled_hint_line(
    const LiveTheme& theme,
    const std::string& key_text,
    std::string_view description) {
    return theme.foreground(ThemeToken::Dim, key_text) +
        theme.foreground(ThemeToken::Muted, std::format(" {}", description));
}

} // namespace

std::string format_key_text(std::string_view key, bool capitalize) {
    std::string result;
    std::size_t alternative_begin = 0;
    for (std::size_t index = 0; index <= key.size(); ++index) {
        if (index != key.size() && key[index] != '/') continue;
        const auto alternative = key.substr(alternative_begin, index - alternative_begin);
        if (!result.empty()) result.push_back('/');
        bool first_part = true;
        std::size_t part_begin = 0;
        for (std::size_t part_index = 0; part_index <= alternative.size(); ++part_index) {
            if (part_index != alternative.size() && alternative[part_index] != '+') continue;
            const auto part = alternative.substr(part_begin, part_index - part_begin);
            if (!first_part) result.push_back('+');
            first_part = false;
            result += capitalize ? capitalize_part(part) : std::string{part};
            part_begin = part_index + 1;
        }
        alternative_begin = index + 1;
    }
    return result;
}

std::string key_hint(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings,
    std::string_view action,
    std::string_view description) {
    const auto key_text = keybindings.key_text(action);
    return styled_hint_line(
        theme,
        key_text.empty() ? "Unbound" : format_key_text(key_text),
        description);
}

std::string raw_key_hint(
    const LiveTheme& theme,
    std::string_view key,
    std::string_view description) {
    return styled_hint_line(theme, format_key_text(key), description);
}

KeybindingHints::KeybindingHints(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings,
    bool user_bash_available,
    bool clipboard_paste_available)
    : theme_(theme),
      keybindings_(keybindings),
      user_bash_available_(user_bash_available),
      clipboard_paste_available_(clipboard_paste_available) {}

void KeybindingHints::set_expanded(bool expanded) {
    expanded_ = expanded;
}

bool KeybindingHints::expanded() const {
    return expanded_;
}

util::Expected<cch::tui::RenderResult> KeybindingHints::render(std::size_t width) {
    std::string text;
    if (!expanded_) {
        // pi's compact startup instructions, without the logo.
        text += key_hint(theme_, keybindings_, "app.interrupt", "interrupt");
        text += theme_.foreground(
            ThemeToken::Muted,
            std::format(
                " · {} · / commands",
                format_key_text(std::format(
                    "{}/{}",
                    keybindings_.key_text("app.clear"),
                    keybindings_.key_text("app.exit")))));
        if (user_bash_available_) text += theme_.foreground(ThemeToken::Muted, " · ! bash");
        text += theme_.foreground(ThemeToken::Muted, " · ");
        text += key_hint(theme_, keybindings_, "app.tools.expand", "more");
    } else {
        // pi's expanded startup instructions over the assembled subset only
        // (G2: hints render the assembled subset), with no logo.
        const auto append = [&text](std::string line) {
            if (!text.empty()) text.push_back('\n');
            text += std::move(line);
        };
        append(key_hint(theme_, keybindings_, "app.interrupt", "to interrupt"));
        append(key_hint(theme_, keybindings_, "app.clear", "to clear"));
        append(key_hint(theme_, keybindings_, "app.exit", "to exit (empty)"));
        append(key_hint(theme_, keybindings_, "app.tools.expand", "to expand tools"));
        append(key_hint(theme_, keybindings_, "app.thinking.toggle", "to expand thinking"));
        append(key_hint(theme_, keybindings_, "app.message.followUp", "to queue follow-up"));
        append(key_hint(theme_, keybindings_, "app.message.dequeue", "to edit all queued messages"));
        if (clipboard_paste_available_) {
            append(key_hint(
                theme_, keybindings_, "app.clipboard.pasteImage", "to paste image (with text fallback)"));
        }
        append(raw_key_hint(theme_, "/", "for commands"));
        if (user_bash_available_) {
            append(raw_key_hint(theme_, "!", "to run bash"));
            append(raw_key_hint(theme_, "!!", "to run bash (no context)"));
        }
    }

    cch::tui::Text component(std::move(text), 0, 0);
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return rendered;
}

void KeybindingHints::invalidate() {}

} // namespace cch::coding_agent::tui
