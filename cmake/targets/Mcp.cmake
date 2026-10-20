include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Reusable MCP protocol, stdio/streamable-http clients and generic OAuth (pi
# `packages/mcp`). Application configuration persistence, management UI and
# session-tool binding stay in cch_coding_agent and call this owner directly.
cch_parity_declare_target(
    TARGET cch_mcp
    ROLE owner
    OWNER cch_mcp
    SOURCES
        src/mcp/Client.cpp
        src/mcp/HttpClient.cpp
        src/mcp/HttpServerConfig.cpp
        src/mcp/OAuthCallbackServer.cpp
        src/mcp/OAuthDiscovery.cpp
        src/mcp/OAuthFlow.cpp
        src/mcp/OAuthHttpClient.cpp
        src/mcp/OAuthProvider.cpp
        src/mcp/OAuthServerConfig.cpp
        src/mcp/Pkce.cpp
        src/mcp/StdioClient.cpp
        src/mcp/Url.cpp
    DEPENDS
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        OpenSSL::SSL@openssl
        OpenSSL::Crypto@openssl
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_mcp src/mcp/include)
