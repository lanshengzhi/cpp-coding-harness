#include "WriteRenderer.hpp"

#include "PathPresentation.hpp"
#include "RenderUtils.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// pi `formatWriteCall` (`core/tools/renderers/write.ts:96-127`). The body is
/// taken from `args.content`, not from the result, so it is on screen from
/// the first complete argument fragment and the settled result adds nothing.
[[nodiscard]] ToolRenderedText render_write_call(const ToolRenderContext& context) {
    ToolRenderedText rendered;
    rendered.title = bold_foreground(context.theme, ThemeToken::ToolTitle, "write") + " " +
                     render_tool_path(context.theme, context.cwd, string_argument(context.args, "file_path", "path"));

    // pi `str`: null when the key is present with a non-string value, which is
    // the only way `fileContent === null` is reachable.
    const auto content = string_argument(context.args, "content");
    if (!content.has_value()) {
        // pi `write.ts:108-109`: the invalid-content notice, behind a blank line.
        rendered.blocks.push_back(
                "\n\n" + context.theme.foreground(ThemeToken::Error, "[invalid content arg - expected string]"));
        return rendered;
    }
    // pi `write.ts:110` `else if (fileContent)`: an empty content string is the
    // header and nothing else, with no body and no blank line.
    if (content->empty()) return rendered;

    // pi `write.ts:117`: `maxLines` is the whole file when expanded, the first
    // ten lines otherwise. `fold_head_lines` is pi's `trimTrailingEmptyLines`
    // plus `slice(0, maxLines)`, so `M total` counts real lines.
    constexpr std::size_t kWritePreviewLines = 10;
    const auto max_lines = context.expanded ? std::numeric_limits<std::size_t>::max() : kWritePreviewLines;
    const auto fold = fold_head_lines(normalize_display_text(*content), max_lines);

    // pi `write.ts:120`: plain text, no highlighting, tabs as three spaces.
    std::string body = "\n\n" + join_rendered_rows(fold.lines, [&context](const std::string& line) {
        return context.theme.foreground(ThemeToken::ToolOutput, replace_tabs(line));
    });
    // pi `write.ts:122`: write is the one renderer whose hint carries the
    // total, which is what distinguishes it from read's.
    if (fold.remaining > 0) body += fold_hint(context, fold.remaining, fold.lines.size() + fold.remaining);
    rendered.blocks.push_back(std::move(body));
    return rendered;
}

/// pi `formatWriteResult` (`write.ts:128-143`).
[[nodiscard]] ToolRenderedText render_write_result(const ToolRenderedResult& result, const ToolRenderContext& context) {
    // pi `write.ts:132-134`: a successful write renders nothing at all, because
    // the call half already showed the content. This is the duplicate the
    // write renderer exists to remove, not a skipped step.
    if (!context.is_error) return {};
    // pi `write.ts:139-141`: an error with no text renders nothing either.
    if (result.output.empty()) return {};
    return ToolRenderedText{
            .blocks = {"\n" + context.theme.foreground(ThemeToken::Error, result.output)},
    };
}

} // namespace

ToolRenderer make_write_renderer() {
    return ToolRenderer{
            .render_call = render_write_call,
            .render_result = render_write_result,
    };
}

} // namespace cch::coding_agent::tui
