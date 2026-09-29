#pragma once

#include <cch/support/JsonValue.hpp>

#include <string>

namespace cch::mcp {

/// One tool an Upstream MCP Server advertises, in the passive form the MCP
/// Host publishes across its Owner Interface (ADR 0065). The wire DTO that
/// `tools/list` decodes into this value stays private to the package, and
/// Qualified Tool Name namespacing happens downstream in `SessionFactory`.
///
/// ADR 0064 / `CONTEXT.md`: the descriptor is the Upstream's own tool name
/// and JSON Schema; the Server Id that namespaces it is configuration, not
/// part of the advertised tool.
struct UpstreamToolDescriptor {
    std::string name{};
    std::string description{};
    cch::support::JsonValue parameters{};
};

} // namespace cch::mcp
