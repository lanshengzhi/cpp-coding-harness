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
# package currently compiles only its interface anchor; the wire, transport,
# and connection behavior arrive in their own slices.
cch_parity_declare_target(
    TARGET cch_mcp
    ROLE owner
    OWNER cch_mcp
    SOURCES
        src/mcp/McpHost.cpp
    DEPENDS
        cch_support
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_mcp src/mcp/include)
