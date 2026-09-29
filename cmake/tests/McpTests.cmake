include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # cch_mcp (issues #836, #837)
    #
    # Every case here drives the full client stack above the one injected
    # transport seam (`tests/support/ScriptedMcpTransport.hpp`), so the shard
    # covers framing, `_meta`/header injection, the era probe, the defensive
    # catalog rules, the defensive matrix, and the v1 notification policy
    # without a second seam and without a socket. The Streamable HTTP cases
    # swap the scripted transport for the production Beast TLS transport and
    # talk to the test-only local MCP HTTP server
    # (`tests/support/LocalMcpHttpServer.hpp`) over a real socket, which is
    # the same seam reached the other way.
    add_executable(cch_tests_mcp
        tests/Catch2Main.cpp
        tests/support/LocalMcpHttpServer.cpp
        tests/mcp/CatalogDefenseTest.cpp
        tests/mcp/DefensiveMatrixTest.cpp
        tests/mcp/JsonRpcFramingTest.cpp
        tests/mcp/NotificationIgnoreTest.cpp
        tests/mcp/StreamableHttpTransportTest.cpp
        tests/mcp/UpstreamAuthTest.cpp
        tests/mcp/UpstreamClientStackTest.cpp
        tests/mcp/WireContractTest.cpp
)
    target_include_directories(cch_tests_mcp PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_mcp
        PRIVATE
            cch_mcp
            cch_support
            Boost::headers
            Catch2::Catch2
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
)
    target_compile_definitions(cch_tests_mcp PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_mcp PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_mcp ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_mcp ${CCH_PARITY_BUILD_GATE_TARGET})
