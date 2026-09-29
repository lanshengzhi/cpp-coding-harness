// Package translation-unit anchor for the MCP Host Capability Owner Package
// (ADR 0065). The package ships its Owner Interface
// (`<cch/mcp/...>`) and its Parity Architecture Gate evidence; the wire layer,
// transport, and connection machinery land in the follow-up slices, so this
// translation unit defines nothing yet.
//
// Including the Owner Interface here is deliberate: it gives the Gate a
// declared `cch_mcp` source whose direct-include evidence proves the
// canonical `<cch/mcp/...>` spelling resolves from inside the package, and it
// gives the Owner Interface standalone-compile evidence a real consumer.

#include <cch/mcp/UpstreamTool.hpp>
