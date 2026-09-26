#include "BashRenderer.hpp"

namespace cch::coding_agent::tui {

ToolRenderer make_bash_renderer() {
    return ToolRenderer{
            .render_call = [](const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/bash.ts renderCall; implemented by #826.
                return {};
            },
            .render_result = [](const ToolRenderedResult&, const ToolRenderContext&) -> ToolRenderedText {
                // pi renderers/bash.ts renderResult; implemented by #826.
                return {};
            },
    };
}

} // namespace cch::coding_agent::tui
