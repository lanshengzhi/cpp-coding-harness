#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/util/Error.hpp>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {

[[nodiscard]] inline util::Expected<std::reference_wrapper<Component>> attach_child(
    std::vector<std::unique_ptr<Component>>& children,
    std::unique_ptr<Component> component,
    std::string_view owner) {
    if (!component) {
        const auto message = owner.empty()
                                 ? std::string("TUI cannot attach a null Component")
                                 : std::format("TUI {} cannot attach a null Component", owner);
        return std::unexpected(util::make_error(util::ErrorCode::Validation, message));
    }
    auto& child = *component;
    children.push_back(std::move(component));
    return child;
}

[[nodiscard]] inline util::Expected<std::string> apply_background(
    BackgroundHook& background_hook,
    std::string line,
    std::size_t width,
    std::string_view owner) {
    const auto input_width = visible_width(line);
    const auto has_background = static_cast<bool>(background_hook);
    if (background_hook) {
        try {
            line = background_hook(std::move(line));
        } catch (const std::exception&) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                std::format("TUI {} background hook failed", owner),
                "the background callback threw an exception"));
        } catch (...) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                std::format("TUI {} background hook failed", owner),
                "the background callback threw an unknown exception"));
        }
    }
    auto prepared = prepare_rendered_line(line, width);
    if (!prepared) return std::unexpected(prepared.error());
    if (has_background && visible_width(*prepared) != input_width) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::format("TUI {} background hook changed visible width", owner)));
    }
    return prepared;
}

} // namespace cch::tui::detail
