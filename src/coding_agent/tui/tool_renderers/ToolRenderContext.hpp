#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cch::coding_agent::tui {

/// pi `ToolRenderContext` (`core/extensions/types.ts:425-450`) narrowed to
/// what a string-out renderer needs: the live presentation state a renderer
/// reads, owned by the host `ToolExecutionComponent` and valid only for the
/// duration of one renderer's call.
///
/// pi's `args` is the tool's parsed argument object. The C++ host parses the
/// accumulated argument text with an exact JSON parse, so a half-streamed
/// argument object is an empty object rather than a partial one: a title never
/// renders a growing JSON fragment. pi's `state` (per-render scratch for
/// component reuse) and `lastComponent` have no counterpart because the seam
/// returns strings and owns no components; pi keeps the execution clock in
/// that scratch, while this host instead receives the duration the agent loop
/// recorded on the settled result (pi `durationMs`), which also survives a
/// resume (#812).
struct ToolRenderContext {
    /// pi `args`: the tool call arguments. The host owns this value.
    const support::JsonValue& args;
    /// pi `ToolRenderContext.toolName` as a value: the fallback renderer needs
    /// the name it renders for, which pi's per-tool renderers know by being
    /// registered under it.
    std::string_view tool_name;
    /// pi `toolCallId`: stable across the call and result renders of one tool
    /// execution, so a renderer can key caches on it.
    std::string_view tool_call_id;
    /// pi `cwd`: the working directory the tool execution runs in. Paths are
    /// resolved against it before they are linked or classified.
    std::string_view cwd;
    /// The visible content width the rendered text is wrapped at, which is
    /// the width the host was last rendered at less the tool block's own box
    /// padding. pi reaches the same number in the renderer's
    /// `render: (width) => ...` callback at *render* time; this seam returns
    /// strings at rebuild time, so the host publishes the width it will
    /// measure, keyed exactly as pi keys its fold cache
    /// (`state.cachedWidth !== width`). The default is the 80-column terminal
    /// pi assumes, used only by a rebuild that precedes the first render.
    /// Only the fold-capable renderers read it; the rest ignore it.
    std::size_t width{80};
    /// pi `theme`, passed through the context so a renderer needs one input.
    const LiveTheme& theme;
    /// The `app.tools.expand` key text, or `Unbound` when nothing is bound.
    /// pi prints the empty key text in that case; `Unbound` is this
    /// repository's existing divergence (`KeybindingHints.cpp`). The viewed
    /// string is host-owned and valid for the duration of the call, like
    /// `args`.
    std::string_view expand_key;
    /// pi `keyHint("app.tools.expand", "to expand")`, resolved once by the
    /// host: `dim(expand_key) + muted(" to expand")`. The viewed string is
    /// host-owned and valid for the duration of the call, like `args`.
    std::string_view expand_hint;
    /// pi `argsComplete`: the streamed arguments stopped growing.
    bool args_complete{false};
    /// pi `executionStarted`: the agent began running the tool.
    bool execution_started{false};
    /// pi `isPartial`: the result view is still streaming.
    bool is_partial{true};
    /// pi `isError`: the settled result is an error. False until a result
    /// exists.
    bool is_error{false};
    /// pi `expanded`: the result view is expanded.
    bool expanded{false};
    /// pi `ToolRenderContext.durationMs`: the execution duration the agent
    /// loop recorded on the settled result (`ToolResultMessage.durationMs`).
    /// pi exposes `this.isPartial ? undefined : this.result?.durationMs`, so
    /// no duration line renders before the settled result arrives; the host
    /// mirrors that by leaving this nullopt while `is_partial` holds. Absent
    /// for legacy results and never-executed calls, and identical after a
    /// resume because the recorded value, not a wall-clock measurement, feeds
    /// the renderer (#812).
    std::optional<std::int64_t> duration_ms{std::nullopt};
};

} // namespace cch::coding_agent::tui
