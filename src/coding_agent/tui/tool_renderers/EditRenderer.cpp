#include "EditRenderer.hpp"

#include "PathPresentation.hpp"
#include "RenderUtils.hpp"
#include "coding_agent/tui/DiffRenderer.hpp"

#include <cch/support/JsonValue.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::tui {
namespace {

/// pi `formatEditCall` (`core/tools/renderers/edit.ts:83-86`): the header only.
/// The call half carries no argument JSON — pi's `edit` shows the pre-execution
/// diff preview here, gated on `context.argsComplete`, and the preview is
/// out of scope for this spec because it would put file IO in the presentation
/// layer. With no preview there is nothing between the header and the result
/// half's diff.
[[nodiscard]] ToolRenderedText render_edit_call(const ToolRenderContext& context) {
    return ToolRenderedText{
            .title = bold_foreground(context.theme, ThemeToken::ToolTitle, "edit") + " " +
                     render_tool_path(context.theme, context.cwd, string_argument(context.args, "file_path", "path")),
    };
}

/// pi `edit.ts:108` `result.details?.diff`, read as the display diff string
/// `AsyncToolFactories.cpp:452` writes.
[[nodiscard]] std::optional<std::string_view> result_diff(const ToolRenderedResult& result) {
    if (!result.details.has_value()) return std::nullopt;
    const auto* object = result.details->get_if<support::JsonValue::object_t>();
    if (object == nullptr) return std::nullopt;
    const auto found = object->find("diff");
    if (found == object->end()) return std::nullopt;
    const auto* text = found->second.get_if<std::string>();
    if (text == nullptr) return std::nullopt;
    return std::string_view{*text};
}

/// pi `formatEditResult` (`edit.ts:87-114`).
///
/// Every block this returns is `\n`-prefixed. In pi the blank row is not in
/// the string at all: `edit.ts:234` adds `new Spacer(1)` to the result
/// container *after* the `if (!output) return` gate, so it precedes whatever
/// text the result half produced — diff or error alike. This seam's host joins
/// `title` and the blocks with no separator of its own
/// (`ToolExecutionComponent.cpp:117`), so the renderer owns that leading
/// newline here. The output matches pi; the responsibility for the blank row
/// does not, which is a fact about the seam worth recording in the ADR.
[[nodiscard]] ToolRenderedText render_edit_result(const ToolRenderedResult& result, const ToolRenderContext& context) {
    if (context.is_error) {
        // pi `edit.ts:97-106`: no text renders nothing. pi's
        // `errorText === previewError` suppression is part of the deferred
        // pre-execution preview and is dead without it.
        if (result.output.empty()) return {};
        return ToolRenderedText{
                .blocks = {"\n" + context.theme.foreground(ThemeToken::Error, result.output)},
        };
    }
    // pi `edit.ts:107-111`: the success path renders `details.diff`, and
    // suppresses it only when the call half is already showing the same diff
    // from the pre-execution preview. That preview is out of scope, so there
    // is never a `previewDiff` here and the suppression guard is deliberately
    // not ported: it would hide the diff the moment a preview is added.
    const auto diff = result_diff(result);
    if (!diff.has_value()) return {};
    return ToolRenderedText{
            .blocks = {"\n" + render_diff(context.theme, *diff)},
    };
}

} // namespace

ToolRenderer make_edit_renderer() {
    return ToolRenderer{
            .render_call = render_edit_call,
            .render_result = render_edit_result,
    };
}

} // namespace cch::coding_agent::tui
