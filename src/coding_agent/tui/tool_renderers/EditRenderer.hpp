#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderer.hpp"

namespace cch::coding_agent::tui {

/// The pi `edit` renderer pair (`core/tools/renderers/edit.ts`), registered by
/// name as `edit`.
[[nodiscard]] ToolRenderer make_edit_renderer();

} // namespace cch::coding_agent::tui
