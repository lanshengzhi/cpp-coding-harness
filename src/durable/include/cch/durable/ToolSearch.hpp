#pragma once

#include <cch/durable/Workflow.hpp>

namespace cch::durable {

/// Durable deferred-tool discovery (frozen pi `createToolSearchExtension`).
/// Loaded names persist in the conversation's `pi.agent:v1` `modelTools.add`.
[[nodiscard]] Extension tool_search_extension();

} // namespace cch::durable
