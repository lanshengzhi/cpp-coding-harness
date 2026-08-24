include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # Aggregate target that builds every shard; a build convenience, not a
    # scheduling authority (issue #447; CODING_STANDARDS.md section 11.6).
    add_custom_target(pike_tests
        DEPENDS
            cch_tests_support
            cch_tests_tui
            cch_tests_ai
            cch_tests_ai_async_bridge
            cch_tests_agent
            cch_tests_harness_tools
            cch_tests_coding_agent
            cch_tests_coding_agent_interactive
            cch_tests_cli_arch
            ${CCH_PARITY_BUILD_GATE_TARGET}
)
