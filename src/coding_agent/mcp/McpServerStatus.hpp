#pragma once

#include <string>

namespace cch::coding_agent::mcp {

/// One MCP server's lifecycle state at Session Assembly (pi `McpServer`'s
/// `connection.state`, spec #865 ticket #876): `Starting` while a launch is in
/// flight, `Running` once the handshake and `tools/list` succeed, `Stopped` for
/// a server disabled in `mcp.json`, and `Failed` when the launch, handshake, or
/// tool listing fails. Assembly is synchronous, so `Starting` is transient and
/// not observable on a completed session; the state is part of the lifecycle
/// vocabulary the status list reports.
enum class McpServerState { Starting, Running, Stopped, Failed };

/// One server's reported lifecycle state, surfaced to the user through the
/// creation result's status list and the per-state Session diagnostics.
struct McpServerStatus {
    /// The server namespace (`mcp__<name>__`).
    std::string name;
    McpServerState state{McpServerState::Stopped};
    /// Human-readable detail: the defining `mcp.json`, the tool count when
    /// running, or the failure reason when failed.
    std::string detail;
};

} // namespace cch::coding_agent::mcp
