#include "ReadRenderer.hpp"

namespace cch::coding_agent::tui {

ToolRenderer make_read_renderer() {
    return ToolRenderer{
            .render_call = [](const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/read.ts renderCall; implemented by #825.
                return {};
            },
            .render_result = [](const ToolRenderedResult&, const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/read.ts renderResult; implemented by #825.
                return {};
            },
    };
}

} // namespace cch::coding_agent::tui
