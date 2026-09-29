#pragma once

// Poison case for the Owner Interface standalone-compile evidence (issue
// #834): an illegal Owner edge *out of* `cch_mcp`. The MCP Host package
// declares no legal Owner dependency, so `<cch/ai/...>` is not on its include
// path (ADR 0065).

#include <cch/ai/Tool.hpp>
