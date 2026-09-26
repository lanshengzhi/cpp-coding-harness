#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderContext.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/Text.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

[[nodiscard]] std::vector<std::string> split_lines(std::string_view text);

/// pi `theme.bold` under a foreground token: the tool-title bold every call
/// header and the call fallback use.
[[nodiscard]] std::string bold_foreground(const LiveTheme& theme, ThemeToken token, std::string_view text);

/// pi `render-utils.ts:25` `str` over one argument key, with pi's
/// `args?.file_path ?? args.path` precedence: the first key that is present
/// wins. The result is the string argument, an empty view when the key is
/// absent or JSON null, and nullopt when the key is present with a non-string
/// value — pi's `[invalid arg]` case.
[[nodiscard]] std::optional<std::string_view> string_argument(
        const support::JsonValue& args, std::string_view primary_key, std::string_view secondary_key = {});

/// pi `render-utils.ts:31` `replaceTabs`: a tab renders as three spaces.
[[nodiscard]] std::string replace_tabs(std::string_view text);

/// pi `render-utils.ts:35` `normalizeDisplayText`: drop carriage returns.
[[nodiscard]] std::string normalize_display_text(std::string_view text);

/// pi's `trimTrailingEmptyLines` + `lines.slice(0, maxLines)` head fold, the
/// split every call renderer that folds from the top performs (read, write,
/// and the fallback). Bash folds from the tail and does not use this.
struct HeadFold {
    /// The kept lines, after trailing empty lines were dropped so `remaining`
    /// counts real lines only.
    std::vector<std::string> lines{};
    /// Lines the fold dropped.
    std::size_t remaining{0};
};

[[nodiscard]] HeadFold fold_head_lines(std::string_view text, std::size_t max_lines);

/// pi `modes/interactive/components/visual-truncate.ts:8` `truncateToVisualLines`:
/// keep the **last** `max_visual_lines` *visual* lines — lines are measured
/// after wrapping at `width`, so one long logical line that wraps to three
/// rows is three, not one. `skipped` is the number of **visual** lines dropped
/// from the front, which is the count pi's earlier-lines hint reports.
struct VisualFold {
    /// The kept tail, in render order, each row already styled.
    std::vector<std::string> lines{};
    /// Visual lines dropped from the front.
    std::size_t skipped{0};
};

/// pi's `truncateToVisualLines`. `padding_x` is pi's `paddingX`, which is `0`
/// inside the tool block's box and `1` inside a plain container. Rendering
/// can fail on a width too small for the text, which the caller propagates
/// rather than silently folding an empty tail.
[[nodiscard]] support::Expected<VisualFold> fold_tail_visual_lines(
        std::string_view text, std::size_t max_visual_lines, std::size_t width, std::size_t padding_x = 0);

/// pi's uniform fold hint `... (N more lines, <key> to expand)`. `total_lines`
/// adds pi's write variant `... (N more lines, M total, <key> to expand)`.
[[nodiscard]] std::string fold_hint(
        const ToolRenderContext& context, std::size_t remaining, std::optional<std::size_t> total_lines = std::nullopt);

} // namespace cch::coding_agent::tui
