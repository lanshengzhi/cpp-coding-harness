#pragma once

#include <cch/mcp/UpstreamTool.hpp>

#include <string>
#include <vector>

namespace cch::mcp {

/// What a `server/discover` probe learned about one Upstream MCP Server.
///
/// The era probe fixes the protocol revision for the whole connection, so a
/// probe result whose revision this build does not speak, whose required
/// fields are missing, or whose declared capability set this build does not
/// implement is rejected outright rather than partially believed.
///
/// `name` is the server's self-reported display name and never an identity:
/// the Server Id from configuration is the sole stable identity
/// (`CONTEXT.md`).
struct UpstreamServerInfo {
    std::string name{};
    std::string version{};
    /// The server capabilities this build understands, sorted by name. An
    /// unrecognized declared capability fails the probe.
    std::vector<std::string> capabilities{};
    /// Server-provided usage guidance surfaced to the model (spec #833
    /// story 18). Absent when the server offers none.
    std::string instructions{};
};

/// One Upstream MCP Server's tool catalog as the MCP Host holds it, in the
/// server's own `tools/list` order (servers SHOULD return a deterministic
/// order, and the catalog cache preserves it).
struct UpstreamCatalog {
    std::vector<UpstreamToolDescriptor> tools{};
    std::string instructions{};
};

} // namespace cch::mcp
