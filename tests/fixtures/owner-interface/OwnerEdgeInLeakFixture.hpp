#pragma once

// Poison case for the Owner Interface standalone-compile evidence (issue
// #834): an illegal Owner edge *into* `cch_mcp`. The TUI Owner Package declares
// no legal Owner dependency, so `<cch/tui/...>` is not on its include path
// (ADR 0065).

#include <cch/tui/Style.hpp>
