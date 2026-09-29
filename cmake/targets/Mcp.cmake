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
# (issue #836). The package's only network source is the Streamable HTTP
# transport in `src/mcp/transport/`, a TLS-only Beast client over the same
# concrete-executor discipline as the `cch_ai` client transports (ADR 0054);
# it depends on no code in `cch_ai` (ADR 0065).
cch_parity_declare_target(
    TARGET cch_mcp
    ROLE owner
    OWNER cch_mcp
    SOURCES
        src/mcp/EraAdapter.cpp
        src/mcp/HeaderMirror.cpp
        src/mcp/JsonRpc.cpp
        src/mcp/McpHost.cpp
        src/mcp/Redaction.cpp
        src/mcp/SseResponseStream.cpp
        src/mcp/UpstreamAuth.cpp
        src/mcp/UpstreamClient.cpp
        src/mcp/UpstreamConnection.cpp
        src/mcp/WireDto.cpp
        src/mcp/transport/BoostBeastStreamableHttpTransport.cpp
        src/mcp/transport/RetryPolicy.cpp
    DEPENDS
        cch_support
        Boost::headers@boost
        OpenSSL::SSL@openssl
        OpenSSL::Crypto@openssl
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_mcp src/mcp/include)
