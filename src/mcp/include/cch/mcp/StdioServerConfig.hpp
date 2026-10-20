#pragma once

// Launch descriptor for one MCP server over stdio (`StdioTransportOptions` in
// pi `packages/mcp/src/transports/stdio.ts`). Application configuration
// (`mcp.json` exposure, description and persistence) stays in the application
// owner and owns the surrounding entry, not this transport value.

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace cch::mcp {

struct StdioServerConfig {
    /// pi server name: the namespace of the server's tools and the identity
    /// used in diagnostics.
    std::string name;
    /// Executable to launch (resolved against `PATH`).
    std::string command;
    /// Arguments passed after `command` (argv[0] is the resolved executable).
    std::vector<std::string> args;
    /// Environment variables layered over the inherited environment, so a
    /// configured entry wins. Empty keeps the inherited environment.
    std::map<std::string, std::string> env;
    /// Longest a single request may wait for its response before failing with
    /// a timeout (pi `DEFAULT_REQUEST_TIMEOUT_MS`). A value of zero or less
    /// disables the deadline.
    std::chrono::milliseconds request_timeout{std::chrono::milliseconds{30000}};
};

} // namespace cch::mcp
