#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {

/// pi `core/tools/render-utils.ts:10` `shortenPath`: a home-prefixed path
/// renders as `~` + the remainder. It is home shortening only — pi does not
/// make paths cwd-relative here, and neither does this.
[[nodiscard]] std::string shorten_path(std::string_view path);

/// pi `render-utils.ts:75` `renderToolPath`: the accent-coloured, `~`-shortened
/// path a read/write/edit title shows, wrapped in an OSC 8 `file://` hyperlink
/// when the terminal reports hyperlink support. `raw_path` is the call's raw
/// path argument, where nullopt is pi's `null` (the argument is present and not
/// a string, which renders `[invalid arg]`) and an empty view is an absent or
/// null argument (which renders `...`, the progressive slot pi shows while the
/// arguments are still streaming).
[[nodiscard]] std::string render_tool_path(
        const LiveTheme& theme, std::string_view cwd, std::optional<std::string_view> raw_path);

} // namespace cch::coding_agent::tui
