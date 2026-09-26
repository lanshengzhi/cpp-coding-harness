#include "EditRenderer.hpp"

namespace cch::coding_agent::tui {

ToolRenderer make_edit_renderer() {
    return ToolRenderer{
            .render_call = [](const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/edit.ts renderCall; implemented by #827.
                return {};
            },
            .render_result = [](const ToolRenderedResult&, const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/edit.ts renderResult; implemented by #827.
                return {};
            },
    };
}

} // namespace cch::coding_agent::tui
