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

    # Catch discovery appends its generated test includes as each shard is
    # declared. Append the classification include last so it sees every
    # discovered test and can add the default `spec` label without replacing
    # the module, issue, and explicit de-pi labels (issue #625).
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/TestClassification.cmake")
