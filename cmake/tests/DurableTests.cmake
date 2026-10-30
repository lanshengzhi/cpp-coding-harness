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
        tests/durable/TerminalLiveBatchTest.cpp
        tests/durable/DocumentLifecycleTest.cpp
        tests/durable/SessionTransactionTest.cpp
        tests/durable/SessionReadOrderTest.cpp
        tests/durable/SchedulerVisibilityTest.cpp
        tests/durable/SessionWatchTest.cpp
        tests/durable/SubmissionStorageTest.cpp
        tests/durable/TaskExecutionTest.cpp
        tests/durable/CrashRecoveryTest.cpp
        tests/durable/HarnessWorkflowTest.cpp
        tests/durable/HarnessSubmissionsTest.cpp
        tests/durable/HarnessOwnershipTest.cpp
        tests/durable/HarnessFailureLifecycleTest.cpp
        # Hidden `[.]` probe pair: deliberate RED for the -fno-exceptions
        # failure path (issue #1167); never selected by a discovery spec.
        tests/durable/HarnessFailureCleanupProbeTest.cpp
        tests/durable/HarnessObservationTest.cpp
        tests/durable/HarnessImagesTest.cpp
        tests/durable/HarnessCodemodeTest.cpp
        tests/durable/HarnessToolSearchTest.cpp
        tests/durable/ImagesTest.cpp
        tests/support/ImagePixels.cpp
)
    target_include_directories(cch_tests_durable PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_include_directories(cch_tests_durable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    target_link_libraries(cch_tests_durable
        PRIVATE
            cch_durable
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_durable PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_durable PRIVATE ${CCH_WARNING_OPTIONS})
    # The deliberate-RED cleanup probe ([.][issue1167]) stays hidden from every
    # discovery spec; it runs only when a spec names its tag explicitly.
    # Bound the whole live continuation process, including Harness operations whose
    # completion or cleanup cannot be interrupted by a waiter's stop token.
    catch_discover_tests(cch_tests_durable TEST_SPEC "~[issue1086live]~[issue1084]~[issue1094]" ADD_TAGS_AS_LABELS)
    catch_discover_tests(cch_tests_durable TEST_SPEC "[issue1094]" PROPERTIES TIMEOUT 30 ADD_TAGS_AS_LABELS)
    catch_discover_tests(cch_tests_durable TEST_SPEC "[issue1084]" PROPERTIES TIMEOUT 90 ADD_TAGS_AS_LABELS)
    catch_discover_tests(cch_tests_durable TEST_SPEC "[issue1086live]" PROPERTIES TIMEOUT 90 ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_durable ${CCH_PARITY_BUILD_GATE_TARGET})
