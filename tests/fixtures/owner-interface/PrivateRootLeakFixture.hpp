#pragma once

// Poison case for the Owner Interface standalone-compile evidence (issue
// #834): the package's private `src/` root is not published, so an Owner
// Interface header may not reach into it (ADR 0039 section 12.3).

#include <mcp/McpHost.cpp>
