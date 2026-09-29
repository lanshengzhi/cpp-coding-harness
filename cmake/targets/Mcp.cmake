include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# MCP Host Capability Owner Package (ADR 0064, ADR 0065; issue #834). Owns the
# MCP wire layer, the Streamable HTTP transport, per-Upstream connection state,
# the MRTR/Pending Elicitation values, defensive limits, and upstream
# credentials. It is client-only: pike never exposes an MCP endpoint.
#
# The package depends on nothing but the pi-neutral `cch_support` package, and
# its Owner Interface carries passive value contracts only — never a
# Boost.Asio/Beast type and never an exception type (ADR 0042, ADR 0046). The
# protocol-version string and the reserved `io.modelcontextprotocol/*` `_meta`
# keys live behind the one private constants point `src/mcp/Protocol.hpp`
# (issue #836). No source here opens a socket: the Streamable HTTP transport
# arrives in its own slice above the injected `McpTransport` seam.
cch_parity_declare_target(
    TARGET cch_mcp
    ROLE owner
    OWNER cch_mcp
    SOURCES
        src/mcp/EraAdapter.cpp
        src/mcp/HeaderMirror.cpp
        src/mcp/JsonRpc.cpp
        src/mcp/McpHost.cpp
        src/mcp/UpstreamClient.cpp
        src/mcp/WireDto.cpp
    DEPENDS
        cch_support
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_mcp src/mcp/include)
