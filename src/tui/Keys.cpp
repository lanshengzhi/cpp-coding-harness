#include <cch/tui/Keys.hpp>

#include "tui/InputInternal.hpp"

#include <cch/support/Error.hpp>
#include <array>
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
    identifier += event.key;
    return identifier;
}

bool matches_key(const KeyEvent& event, std::string_view identifier) {
    const auto parsed = parse_key_id(identifier);
    return parsed && parsed->key == event.key && parsed->ctrl == event.ctrl && parsed->shift == event.shift &&
        parsed->alt == event.alt;
}

} // namespace cch::tui
