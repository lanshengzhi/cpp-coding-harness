#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Component.hpp>

#include <memory>
#include <string_view>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi `handleReloadCommand` reload-box message, trimmed of "extensions" for
/// the C++ subset (issue #418, AC2): the bordered box swapped into the
/// editor container while `/reload` runs.
inline constexpr std::string_view kReloadBoxMessage =
    "Reloading keybindings, skills, prompts, themes, and context files...";

/// The `/reload` editor-slot box (pi `handleReloadCommand`): a border rule,
/// spacer, one muted padded line, spacer, border rule. The theme's live
/// palette is captured into self-contained hooks so the box outlives any
/// single palette generation safely.
[[nodiscard]] std::shared_ptr<cch::tui::Component> make_reload_box(
    const LiveTheme& theme);

} // namespace cch::coding_agent::tui
