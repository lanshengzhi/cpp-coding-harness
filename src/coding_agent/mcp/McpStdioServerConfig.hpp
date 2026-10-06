#pragma once

#include <map>
#include <string>
#include <vector>

namespace cch::coding_agent::mcp {

/// Registration surface for one MCP server Pike launches over stdio
/// (`McpStdioServerConfig` in pi `core/mcp-servers.ts`, the fields the
/// `command`/`args`/`env` shape carries). The Session creation request holds
/// the configured list; full server management and persistence (pi `mcp.json`)
/// is a later slice, so this value is only the launch descriptor.
struct McpStdioServerConfig {
    /// pi server name: the namespace of the server's tools
    /// (`mcp__<name>__<tool>`) and the identity used in diagnostics.
    std::string name;
    /// Executable to launch (resolved against `PATH`).
    std::string command;
    /// Arguments passed after `command` (argv[0] is the resolved executable).
    std::vector<std::string> args;
    /// Environment variables layered over the inherited environment, so a
    /// configured entry wins. Empty keeps the inherited environment.
    std::map<std::string, std::string> env;
};

} // namespace cch::coding_agent::mcp
