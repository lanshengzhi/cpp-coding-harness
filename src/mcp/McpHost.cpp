// Package translation-unit anchor for the MCP Host Capability Owner Package
// (ADR 0065). The package ships its Owner Interface (`<cch/mcp/...>`) and its
// Parity Architecture Gate evidence, and this anchor gives the Gate a declared
// `cch_mcp` source whose direct-include evidence proves the canonical
// `<cch/mcp/...>` spelling resolves from inside the package.
//
// The wire layer, the era seam, the defensive catalog rules, and the injected
// transport seam live in their own translation units; this one defines nothing.

#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamClient.hpp>
#include <cch/mcp/UpstreamServer.hpp>
#include <cch/mcp/UpstreamTool.hpp>
#include <cch/mcp/UpstreamToolCall.hpp>
