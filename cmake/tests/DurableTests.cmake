include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # durable
    # The durable owner's public-caller shard: it links the new `cch_durable`
    # owner library and never `cch_coding_agent`, so a regression that
    # reintroduces an application dependency fails here as well as in the
    # Parity Architecture Gate.
    add_executable(cch_tests_durable
        tests/Catch2Main.cpp
        tests/durable/FrozenExpectation.cpp
        tests/durable/JsonObjectOrderTest.cpp
        tests/durable/PromptPlanTest.cpp
        tests/durable/JsonlStorageTest.cpp
        tests/durable/SqliteStorageTest.cpp
        tests/durable/DocumentLifecycleTest.cpp
        tests/durable/SessionTransactionTest.cpp
        tests/durable/SessionReadOrderTest.cpp
        tests/durable/SchedulerVisibilityTest.cpp
        tests/durable/SessionWatchTest.cpp
        tests/durable/SubmissionStorageTest.cpp
        tests/durable/TaskExecutionTest.cpp
        tests/durable/HarnessWorkflowTest.cpp
        tests/durable/HarnessSubmissionsTest.cpp
        tests/durable/HarnessOwnershipTest.cpp
        tests/durable/HarnessFailureLifecycleTest.cpp
        tests/durable/HarnessObservationTest.cpp
)
    target_include_directories(cch_tests_durable PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_durable
        PRIVATE
            cch_durable
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_durable PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_durable PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_durable ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_durable ${CCH_PARITY_BUILD_GATE_TARGET})
