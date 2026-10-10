#pragma once

#include <cch/tui/Keys.hpp>
#include <cch/tui/Style.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tui::detail {

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

/// Remove unmatched opening punctuation wrappers before an autocomplete token
/// (pi's stripLeadingWrappers). A matching closer inside the token means the
/// punctuation is part of the path and must remain searchable.
[[nodiscard]] inline std::string_view strip_leading_wrappers(std::string_view token) {
    while (!token.empty()) {
        char closer = '\0';
        switch (token.front()) {
        case '(':
            closer = ')';
            break;
        case '[':
            closer = ']';
            break;
        case '{':
            closer = '}';
            break;
        case '<':
            closer = '>';
            break;
        case '`':
            closer = '`';
            break;
        default:
            return token;
        }
        if (token.find(closer, 1) != std::string_view::npos) break;
        token.remove_prefix(1);
    }
    return token;
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
[[nodiscard]] support::Expected<std::string> apply_style(
    Hook& hook,
    std::string text,
    std::string_view owner,
    Invoker&& invoke) {
    if (!hook) return text;
    const auto input_width = visible_width(text);
    text = invoke(hook, std::move(text));
    if (visible_width(text) != input_width) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            std::format("TUI {} style hook changed visible width", owner)));
    }
    return text;
}

[[nodiscard]] inline support::Expected<std::string> apply_text_style(
    TextStyleHook& hook,
    std::string text,
    std::string_view owner) {
    return apply_style(hook, std::move(text), owner, [](auto& style, std::string value) {
        return style(std::move(value));
    });
}

[[nodiscard]] inline support::Expected<std::string> apply_selection_style(
    SelectionStyleHook& hook,
    std::string text,
    bool selected,
    std::string_view owner) {
    return apply_style(hook, std::move(text), owner, [selected](auto& style, std::string value) {
        return style(std::move(value), selected);
    });
}

} // namespace cch::tui::detail
