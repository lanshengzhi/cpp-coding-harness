include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # support
    add_executable(cch_tests_support
        tests/Catch2Main.cpp
        tests/support/AllocationCounter.cpp
        tests/support/AsyncResultTest.cpp
        tests/support/ExpectedErrorTest.cpp
        tests/support/ExpectedMacrosTest.cpp
        tests/support/JsonValueIoTest.cpp
        tests/support/TestRunnerIsolationTest.cpp
)
    target_include_directories(cch_tests_support PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_support
        PRIVATE
            cch_support
            glaze::glaze
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_support PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_support PRIVATE ${CCH_WARNING_OPTIONS})
    # Fatal-contract probes (tagged [fatal]) fork a child that must die from
    # SIGABRT. A hang means the probe broke, so they get a tightened hard
    # timeout instead of waiting out the suite-wide default (ADR 0039; #447).
    catch_discover_tests(cch_tests_support TEST_SPEC "~[fatal]" ADD_TAGS_AS_LABELS)
    catch_discover_tests(cch_tests_support TEST_SPEC "[fatal]" PROPERTIES TIMEOUT 60 ADD_TAGS_AS_LABELS)
    # Every test entry point requires fresh successful build-phase Gate evidence
    # (ADR 0039; #470): the shard builds the Gate after all production targets.
    add_dependencies(cch_tests_support ${CCH_PARITY_BUILD_GATE_TARGET})
