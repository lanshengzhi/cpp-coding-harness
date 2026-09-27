#pragma once

#include "coding_agent/tui/tool_renderers/ToolRenderer.hpp"

namespace cch::coding_agent::tui {

/// The pi `bash` renderer pair (`core/tools/renderers/bash.ts`), registered by
/// name as `bash`.
[[nodiscard]] ToolRenderer make_bash_renderer();

} // namespace cch::coding_agent::tui
