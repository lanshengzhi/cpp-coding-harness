#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderer.hpp"

namespace cch::coding_agent::tui {

/// The pi `write` renderer pair (`core/tools/renderers/write.ts`), registered by
/// name as `write`.
[[nodiscard]] ToolRenderer make_write_renderer();

} // namespace cch::coding_agent::tui
