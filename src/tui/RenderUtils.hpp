#pragma once

#include <cch/tui/Component.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {

namespace segment_reset_detail {

/// `kSegmentReset` = `"\x1b[0m"` followed by the shared OSC 8 close, composed at
/// compile time so the close keeps a single definition site (`kOsc8LinkClose`).
inline constexpr auto kBytes = [] {
    constexpr std::string_view kSgrReset{"\x1b[0m"};
    std::array<char, kSgrReset.size() + kOsc8LinkClose.size()> bytes{};
    std::ranges::copy(kSgrReset, bytes.begin());
    std::ranges::copy(kOsc8LinkClose, bytes.begin() + kSgrReset.size());
    return bytes;
}();

} // namespace segment_reset_detail

/// pi's `SEGMENT_RESET` (tui.ts at the frozen baseline): the one full reset a
/// composed row carries, appended after component rendering, after padding,
/// after the background hook, and after overlay compositing.
inline constexpr std::string_view kSegmentReset{
        segment_reset_detail::kBytes.data(), segment_reset_detail::kBytes.size()};

/// Append the one full reset a composed row carries to a single row.
inline void apply_line_reset(std::string& line) { line += kSegmentReset; }

[[nodiscard]] inline support::Expected<std::reference_wrapper<Component>> attach_child(
    std::vector<std::unique_ptr<Component>>& children,
    std::unique_ptr<Component> component,
    std::string_view owner) {
    if (!component) {
        const auto message = owner.empty()
                                 ? std::string("TUI cannot attach a null Component")
                                 : std::format("TUI {} cannot attach a null Component", owner);
        return std::unexpected(support::make_error(support::ErrorCode::Validation, message));
    }
    auto& child = *component;
    children.push_back(std::move(component));
    return child;
}

[[nodiscard]] inline support::Expected<std::string> apply_background(
        BackgroundHook& background_hook, PreparedRenderedLine line, std::size_t width, std::string_view owner) {
    if (!background_hook) return std::move(line.text);

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        line.text = background_hook(std::move(line.text));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception&) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown,
                std::format("TUI {} background hook failed", owner),
                "the background callback threw an exception"));
    } catch (...) {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown,
                std::format("TUI {} background hook failed", owner),
                "the background callback threw an unknown exception"));
    }
#endif

    auto prepared = prepare_rendered_line(line.text, width);
    if (!prepared) return std::unexpected(prepared.error());
    if (prepared->width != line.width) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            std::format("TUI {} background hook changed visible width", owner)));
    }
    return std::move(prepared->text);
}

} // namespace cch::tui::detail
