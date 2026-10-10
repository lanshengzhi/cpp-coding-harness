#include <cch/tui/Keys.hpp>

#include "tui/InputInternal.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace cch::tui {
namespace {

bool is_special_key(std::string_view key) {
    constexpr std::array<std::string_view, 18> kSpecialKeys{
        "escape", "enter", "tab", "space", "backspace", "delete", "insert", "clear", "home",
        "end", "pageUp", "pageDown", "up", "down", "left", "right", "f1", "f2",
    };
    for (const auto candidate : kSpecialKeys) {
        if (candidate == key) return true;
    }
    return key == "f3" || key == "f4" || key == "f5" || key == "f6" || key == "f7" ||
        key == "f8" || key == "f9" || key == "f10" || key == "f11" || key == "f12";
}

std::string normalize_key(std::string_view key) {
    std::string normalized(key);
    for (auto& character : normalized) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    if (normalized == "esc") return "escape";
    if (normalized == "return") return "enter";
    if (normalized == "pageup") return "pageUp";
    if (normalized == "pagedown") return "pageDown";
    return normalized;
}

bool is_baseline_key(std::string_view key) {
    if (is_special_key(key)) return true;
    if (key.size() != 1) return false;
    const auto value = key.front();
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
        detail::is_baseline_symbol(value);
}

bool is_non_printable_key(std::string_view key) {
    // The named keys a printable character never is. Events decoded from a
    // terminal carry their own text and never reach this list; it guards the
    // identity fallback for events built without terminal text.
    constexpr std::array<std::string_view, 21> kNonPrintableKeys{
            "enter",
            "tab",
            "escape",
            "backspace",
            "delete",
            "insert",
            "clear",
            "home",
            "end",
            "pageUp",
            "pageDown",
            "up",
            "down",
            "left",
            "right",
            "menu",
            "capsLock",
            "numLock",
            "scrollLock",
            "pause",
            "printScreen",
    };
    for (const auto candidate : kNonPrintableKeys) {
        if (candidate == key) return true;
    }
    return key.size() > 1 && key.size() <= 3 && key.front() == 'f' &&
           std::all_of(key.begin() + 1, key.end(), [](char value) { return value >= '0' && value <= '9'; });
}

} // namespace

support::Expected<KeyEvent> parse_key_id(std::string_view identifier) {
    if (identifier.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "key identifier is empty"));
    }

    std::size_t key_start = 0;
    if (identifier != "+") {
        const auto separator = identifier.rfind('+');
        if (separator != std::string_view::npos) key_start = separator + 1;
        if (key_start == identifier.size() && identifier.ends_with("++")) --key_start;
    }

    KeyEvent event;
    event.key = normalize_key(identifier.substr(key_start));
    std::size_t start = 0;
    while (start < key_start) {
        const auto separator = identifier.find('+', start);
        if (separator == std::string_view::npos || separator >= key_start) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "key identifier has an empty part"));
        }
        const auto part = identifier.substr(start, separator - start);
        if (part == "ctrl" && !event.ctrl) event.ctrl = true;
        else if (part == "shift" && !event.shift) event.shift = true;
        else if (part == "alt" && !event.alt) event.alt = true;
        else if (part == "super" && !event.super)
            event.super = true;
        else {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "key identifier has an invalid or repeated modifier"));
        }
        start = separator + 1;
    }

    if (!is_baseline_key(event.key)) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "key identifier has an unsupported key"));
    }
    return event;
}

std::string key_id(const KeyEvent& event) {
    std::string identifier;
    if (event.shift) identifier += "shift+";
    if (event.ctrl) identifier += "ctrl+";
    if (event.alt) identifier += "alt+";
    if (event.super) identifier += "super+";
    identifier += event.key;
    return identifier;
}

bool matches_key(const KeyEvent& event, std::string_view identifier) {
    const auto parsed = parse_key_id(identifier);
    return parsed && parsed->key == event.key && parsed->ctrl == event.ctrl && parsed->shift == event.shift &&
           parsed->alt == event.alt && parsed->super == event.super;
}

std::optional<std::string> printable_text(const KeyEvent& event) {
    // pi `decodePrintableKey`: a repeat inserts the same text as a press;
    // only a release never carries text. Gate on the release directly — not
    // on `carries_press_behavior`, which only press events set currently.
    if (event.type == KeyEventType::Release) return std::nullopt;
    if (event.ctrl || event.alt || event.super || event.key.empty()) return std::nullopt;
    if (!event.text.empty()) return event.text;
    if (is_non_printable_key(event.key)) return std::nullopt;
    if (event.key == "space") return " ";
    // The identifier grammar lowercases shift-modified letters, so the typed
    // case is restored here for events decoded without terminal text.
    if (event.shift && event.key.size() == 1) {
        const auto letter = static_cast<unsigned char>(event.key.front());
        if (letter >= 'a' && letter <= 'z') {
            return std::string(1, static_cast<char>(letter - 'a' + 'A'));
        }
    }
    return event.key;
}

} // namespace cch::tui
