#pragma once

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

struct VisibleRange {
    std::size_t begin{0};
    std::size_t end{0};
};

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
