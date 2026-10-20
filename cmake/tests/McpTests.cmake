include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # The MCP owner's public-caller shard: it links the reusable library, never
    # cch_coding_agent, so a regression that reintroduces an application
    # dependency fails here as well as in the Parity Architecture Gate. The
    # caller drives initialize/list/call/close against local fixture servers
    # and constructs no application Session.
    add_executable(cch_tests_mcp
        tests/Catch2Main.cpp
        tests/mcp/McpOwnerTest.cpp
)
    target_include_directories(cch_tests_mcp PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_mcp
        PRIVATE
            cch_mcp
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_mcp PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_mcp PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_mcp ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_mcp ${CCH_PARITY_BUILD_GATE_TARGET})
