#include "ToolRendererRegistry.hpp"

#include "BashRenderer.hpp"
#include "EditRenderer.hpp"
#include "ReadRenderer.hpp"
#include "RenderUtils.hpp"
#include "WriteRenderer.hpp"
#include "coding_agent/PrettyJson.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

/// pi `tool-execution.ts:40` `FALLBACK_PREVIEW_LINES`.
constexpr std::size_t kFallbackPreviewLines = 10;

/// pi `formatToolExecution` (`tool-execution.ts:421-432`), call half: the bold
/// tool name, a blank line, then the arguments as pretty-printed JSON.
[[nodiscard]] ToolRenderedText render_fallback_call(const ToolRenderContext& context) {
    ToolRenderedText rendered;
    rendered.title = bold_foreground(context.theme, ThemeToken::ToolTitle, context.tool_name);
    if (auto arguments = detail::serialize_pretty_json(context.args, false); arguments) {
        rendered.blocks.push_back(std::format("\n\n{}", *arguments));
    }
    return rendered;
}

/// The same fallback's result half: the output folded at
/// `FALLBACK_PREVIEW_LINES` from the top, with pi's remaining-lines hint.
[[nodiscard]] ToolRenderedText render_fallback_result(
        const ToolRenderedResult& result, const ToolRenderContext& context) {
    if (result.output.empty()) return {};
    const auto max_lines = context.expanded ? std::numeric_limits<std::size_t>::max() : kFallbackPreviewLines;
    const auto fold = fold_head_lines(result.output, max_lines);
    auto body = join_rendered_rows(fold.lines,
            [&context](const std::string& line) { return context.theme.foreground(ThemeToken::ToolOutput, line); });
    if (fold.remaining > 0) body += fold_hint(context, fold.remaining);
    return ToolRenderedText{.blocks = {std::move(body)}};
}

} // namespace

ToolRendererRegistry::ToolRendererRegistry()
    : fallback_(ToolRenderer{
              .render_call = render_fallback_call,
              .render_result = render_fallback_result,
      }) {}

ToolRendererRegistry::ToolRendererRegistry(ToolRendererRegistry&&) noexcept = default;
ToolRendererRegistry& ToolRendererRegistry::operator=(ToolRendererRegistry&&) noexcept = default;
ToolRendererRegistry::~ToolRendererRegistry() = default;

ToolRendererRegistry ToolRendererRegistry::make_default() {
    ToolRendererRegistry registry;
    registry.register_renderer("read", make_read_renderer());
    registry.register_renderer("bash", make_bash_renderer());
    registry.register_renderer("write", make_write_renderer());
    registry.register_renderer("edit", make_edit_renderer());
    return registry;
}

void ToolRendererRegistry::register_renderer(std::string tool_name, ToolRenderer renderer) {
    renderers_.insert_or_assign(std::move(tool_name), std::move(renderer));
}

ToolRenderer& ToolRendererRegistry::lookup(std::string_view tool_name) {
    const auto found = renderers_.find(tool_name);
    return found == renderers_.end() ? fallback_ : found->second;
}

} // namespace cch::coding_agent::tui
