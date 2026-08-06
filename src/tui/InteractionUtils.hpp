#pragma once

#include <cch/tui/Keys.hpp>
#include <cch/tui/Style.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/util/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tui::detail {

/// Printable keys: plain and shift-modified characters only. Named keys and
/// ctrl/alt-modified events are handled by the keybinding actions or
/// rejected, mirroring pi's control-character check.
[[nodiscard]] inline bool is_printable(const KeyEvent& event) {
    if (event.ctrl || event.alt || event.key.empty()) return false;
    return event.key != "enter" && event.key != "tab" && event.key != "escape" &&
        event.key != "backspace" && event.key != "delete" && event.key != "insert" &&
        event.key != "clear" && event.key != "home" && event.key != "end" &&
        event.key != "pageUp" && event.key != "pageDown" && event.key != "up" &&
        event.key != "down" && event.key != "left" && event.key != "right";
}

/// The visible text a printable key event inserts ("space" renders as a
/// space). A shift-modified single ASCII letter renders uppercase, matching
/// pi's "shift+letter produces uppercase" legacy contract (the decoder
/// canonicalizes letters to lowercase for the identifier grammar; the
/// inserted text must preserve the typed case).
[[nodiscard]] inline std::string printable_text(const KeyEvent& event) {
    if (event.key == "space") return " ";
    if (event.shift && event.key.size() == 1) {
        const auto letter = static_cast<unsigned char>(event.key.front());
        if (letter >= 'a' && letter <= 'z') {
            return std::string(1, static_cast<char>(letter - 'a' + 'A'));
        }
    }
    return event.key;
}

struct VisibleRange {
    std::size_t begin{0};
    std::size_t end{0};
};

/// The text after leading ASCII whitespace (pi's trimStart on the editor's
/// ASCII-relevant comparisons).
[[nodiscard]] inline std::string_view trim_start_ascii(std::string_view text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    return text.substr(static_cast<std::size_t>(first - text.begin()));
}

[[nodiscard]] inline VisibleRange centered_visible_range(
    std::size_t total,
    std::size_t selected,
    std::size_t maximum) {
    if (total == 0) return {};
    maximum = std::max<std::size_t>(1, maximum);
    selected = std::min(selected, total - 1);
    const auto begin = std::min(
        selected > maximum / 2 ? selected - maximum / 2 : 0,
        total > maximum ? total - maximum : 0);
    return {.begin = begin, .end = std::min(begin + maximum, total)};
}

template <typename Hook, typename Invoker>
[[nodiscard]] util::Expected<std::string> apply_style(
    Hook& hook,
    std::string text,
    std::string_view owner,
    Invoker&& invoke) {
    if (!hook) return text;
    const auto input_width = visible_width(text);
    try {
        text = invoke(hook, std::move(text));
    } catch (const std::exception&) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            std::format("TUI {} style hook failed", owner),
            "the style callback threw an exception"));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            std::format("TUI {} style hook failed", owner),
            "the style callback threw an unknown exception"));
    }
    if (visible_width(text) != input_width) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::format("TUI {} style hook changed visible width", owner)));
    }
    return text;
}

[[nodiscard]] inline util::Expected<std::string> apply_text_style(
    TextStyleHook& hook,
    std::string text,
    std::string_view owner) {
    return apply_style(hook, std::move(text), owner, [](auto& style, std::string value) {
        return style(std::move(value));
    });
}

[[nodiscard]] inline util::Expected<std::string> apply_selection_style(
    SelectionStyleHook& hook,
    std::string text,
    bool selected,
    std::string_view owner) {
    return apply_style(hook, std::move(text), owner, [selected](auto& style, std::string value) {
        return style(std::move(value), selected);
    });
}

} // namespace cch::tui::detail
