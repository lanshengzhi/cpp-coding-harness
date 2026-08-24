include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # Suite-wide default hard timeout (ADR 0039; issue #447). CTest kills any
    # test that exceeds it, so a hang in a cancellation, Close, deadlock, or
    # fatal-contract case cannot stall the suite. The [fatal]-selected shard
    # discovery below tightens this further for the fatal probes.
    set(DART_TESTING_TIMEOUT 300 CACHE STRING
        "Default hard timeout (seconds) for every CTest test")
    include(CTest)

    find_package(Catch2 3 CONFIG REQUIRED)
    cch_require_vcpkg_dependency("Catch2" "${Catch2_DIR}")
    include(Catch)

    function(cch_add_supported_build_policy_test case_name should_pass diagnostic_pattern)
        set(test_name "cch_supported_build_policy_${case_name}")
        add_test(
            NAME ${test_name}
            COMMAND
                ${CMAKE_COMMAND}
                -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DCCH_TEST_CASE=${case_name}
                -DCCH_CASE_SHOULD_PASS=${should_pass}
                "-DCCH_DIAGNOSTIC_PATTERN=${diagnostic_pattern}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/SupportedBuildPolicyTest.cmake
        )
        set_tests_properties(${test_name} PROPERTIES LABELS "architecture;build;issue441")
    endfunction()

    # Package-aligned test shard executables (issue #447; CODING_STANDARDS.md
    # section 11.6). Shards are build grouping only: `catch_discover_tests`
    # registers Catch2 cases as individual CTest tests, and CTest names/labels
    # are the sole selection, scheduling, timeout, and reporting authority.
    # Each shard is its own executable, so a focused test edit builds and links
    # only its owning package. `pike_tests` remains as the aggregate
    # target that builds every shard. Grouping follows the Capability Owner or
    # support ownership in CODING_STANDARDS.md section 11.5 and fixture
    # ownership, not equal file counts; no test object is compiled twice.
    # Tests include Owner Interface headers through the owner-local interface
    # roots and private headers through the repository-private src root.
    set(CCH_FORMAL_TEST_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ai/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/agent/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tui/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/coding_agent/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/support/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
