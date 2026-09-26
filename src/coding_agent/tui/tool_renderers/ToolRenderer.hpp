#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderContext.hpp"

#include <cch/support/JsonValue.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

/// What one renderer half returns, narrowed from pi's `Component` to the
/// pieces the host lays out (pi `tool-execution.ts` `updateDisplay`): the call
/// title line, then zero or more body blocks in order beneath it.
///
/// An empty title with no blocks is a real answer, not a skipped step: pi's
/// write renderer returns nothing on success, and a collapsed successful read
/// result renders as the empty string. The host concatenates the title and
/// the blocks into the one text pi's renderer returns as a single component,
/// so a block keeps its own leading newlines — pi's renderers write the
/// `\n\n` between a header and its body themselves.
struct ToolRenderedText {
    std::string title{};
    std::vector<std::string> blocks{};
};

/// The narrowed result pi hands to a result renderer (`tool-execution.ts:350`).
/// `is_error` is not here: renderers read `ToolRenderContext::is_error`.
struct ToolRenderedResult {
    /// Every `text` content block joined with "\n", with `\r` removed. Image
    /// blocks are not text; the host renders them inline, so they stay out of
    /// the result a renderer sees.
    std::string output{};
    /// `result.details` verbatim, or nullopt when the tool emitted none.
    std::optional<support::JsonValue> details{std::nullopt};
};

/// pi `renderCall(args, theme, context) -> Component`. The parsed arguments
/// arrive as `ToolRenderContext::args`.
using ToolRenderCall = std::move_only_function<ToolRenderedText(const ToolRenderContext& context)>;

/// pi `renderResult(result, options, theme, context) -> Component | undefined`.
/// `expanded` and `is_partial` arrive as `ToolRenderContext` fields.
using ToolRenderResult =
        std::move_only_function<ToolRenderedText(const ToolRenderedResult& result, const ToolRenderContext& context)>;

/// pi `ToolRenderers` (`core/tools/renderers/index.ts:19`): the call/result
/// pair registered under one tool name. A renderer is a plain data-in /
/// string-out seam — it never owns a component and never reaches into the
/// host's box.
struct ToolRenderer {
    ToolRenderCall render_call{};
    ToolRenderResult render_result{};
};

} // namespace cch::coding_agent::tui
