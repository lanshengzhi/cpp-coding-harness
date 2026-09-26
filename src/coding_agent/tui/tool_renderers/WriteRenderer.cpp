#include "WriteRenderer.hpp"

namespace cch::coding_agent::tui {

ToolRenderer make_write_renderer() {
    return ToolRenderer{
            .render_call = [](const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/write.ts renderCall; implemented by #827.
                return {};
            },
            .render_result = [](const ToolRenderedResult&, const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/write.ts renderResult; implemented by #827.
                return {};
            },
    };
}

} // namespace cch::coding_agent::tui
