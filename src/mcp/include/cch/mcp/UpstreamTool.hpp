#pragma once

#include <cch/support/JsonValue.hpp>

#include <string>
#include <vector>

namespace cch::mcp {

/// One tool parameter an Upstream MCP Server asked to be mirrored out of the
/// `tools/call` argument object into a request header, declared by the tool's
/// `x-mcp-header` annotation. The mirroring itself is derived privately by
/// `cch_mcp`; only the validated result crosses the Owner Interface.
///
/// The 2026-07-28 revision makes this mirroring mandatory for an annotated
/// parameter, so the entry is part of the descriptor rather than an optional
/// extra. A tool whose annotation is malformed in any way is rejected outright
/// from a `tools/list` result and therefore never appears here: a partially
/// mirrored call would silently drop a header the Upstream requires.
struct UpstreamHeaderParameter {
    /// The Upstream's own parameter name. Its `tools/call` argument value is
    /// what gets mirrored.
    std::string argument_name{};
    /// The request header the argument value is mirrored into, without the
    /// `Mcp-Param-` prefix the transport writes.
    std::string header_name{};
    std::string description{};
    /// The JSON Schema the Upstream declared for this parameter, kept so the
    /// mirroring contract is auditable from the catalog alone.
    support::JsonValue schema{};
    bool required{false};
};

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
    support::JsonValue parameters{};
    std::vector<UpstreamHeaderParameter> header_parameters{};
};

} // namespace cch::mcp
