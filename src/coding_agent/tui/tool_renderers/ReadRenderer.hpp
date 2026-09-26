#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderer.hpp"

namespace cch::coding_agent::tui {

/// The pi `read` renderer pair (`core/tools/renderers/read.ts`), registered by
/// name as `read`.
[[nodiscard]] ToolRenderer make_read_renderer();

} // namespace cch::coding_agent::tui
