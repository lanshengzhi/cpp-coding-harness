#pragma once

#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/tui/Theme.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

/// Passive view of one User Bash execution for Native TUI presentation.
struct BashBlockView {
    std::string command;
    std::string output;
    bool exclude_from_context{false};
    /// The execution is still running: concise status lines are withheld
    /// until the terminal outcome is known.
    bool running{false};
    std::optional<int> exit_code;
    bool cancelled{false};
    bool truncated{false};
    std::optional<std::string> full_output_path;
};

/// Renders the pi-aligned User Bash block shared by the live pending block
/// and committed or resumed transcript entries: a `$ command` header styled
/// by model-context inclusion, a muted output preview derived from the last
/// 20 logical lines and bounded to 20 visual lines unless expanded, and
/// concise cancellation, exit, and truncation status with effective
/// keybinding hints. Behavioral baseline: pi 864b35c bash-execution.ts.
[[nodiscard]] util::Expected<std::vector<std::string>> render_bash_block(
    const LiveTheme& theme,
    const cch::tui::KeybindingRegistry& keybindings,
    const BashBlockView& view,
    bool expanded,
    std::size_t width);

} // namespace cch::coding_agent::tui
