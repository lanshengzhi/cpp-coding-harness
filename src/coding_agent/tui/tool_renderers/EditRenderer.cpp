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

/// pi `edit.ts:97` `result.details?.diff`, read as the display diff string
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
[[nodiscard]] ToolRenderedText render_edit_result(const ToolRenderedResult& result, const ToolRenderContext& context) {
    if (context.is_error) {
        // pi `edit.ts:91-97`: no text renders nothing. pi's
        // `errorText === previewError` suppression is part of the deferred
        // pre-execution preview and is dead without it.
        if (result.output.empty()) return {};
        // pi `edit.ts:98`: no leading newline inside the string, because the
        // host lays the result half out as its own block beneath the title.
        return ToolRenderedText{
                .blocks = {context.theme.foreground(ThemeToken::Error, result.output)},
        };
    }
    // pi `edit.ts:102-105`: the success path renders `details.diff`, and
    // suppresses it only when the call half is already showing the same diff
    // from the pre-execution preview. That preview is out of scope, so there
    // is never a `previewDiff` here and the suppression guard is deliberately
    // not ported: it would hide the diff the moment a preview is added.
    const auto diff = result_diff(result);
    if (!diff.has_value()) return {};
    return ToolRenderedText{
            .blocks = {render_diff(context.theme, *diff)},
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
