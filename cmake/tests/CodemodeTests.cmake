include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # The codemode owner's public-caller shard: it links the reusable library,
    # never cch_coding_agent, so a regression that reintroduces an application
    # dependency fails here as well as in the Parity Architecture Gate.
    add_executable(cch_tests_codemode
        tests/Catch2Main.cpp
        tests/codemode/CodemodeOwnerTest.cpp
        tests/codemode/CodemodeRemoteTest.cpp
        tests/codemode/RemoteWorkerLifetimeTest.cpp
        tests/codemode/RemoteWithTestHooks.cpp
)
    target_include_directories(cch_tests_codemode PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_codemode
        PRIVATE
            cch_codemode
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_codemode PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        # The deterministic worker's lifetime regression (#1232) drives the
        # implementation's private parking seam instead of racing a real
        # retirement; the production archive never defines this.
        CCH_REMOTE_WORKER_TEST_HOOKS
)
    target_compile_options(cch_tests_codemode PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_codemode TEST_SPEC "~[issue1232remote]" ADD_TAGS_AS_LABELS)
    # The lifetime case parks a worker and can abort the process on the recorded
    # defect: a hard timeout is its only backstop, and its gate is process-wide,
    # so it never overlaps another case.
    catch_discover_tests(cch_tests_codemode TEST_SPEC "[issue1232remote]"
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_codemode ${CCH_PARITY_BUILD_GATE_TARGET})

    # The portable no-Node entry's check shard (#1194): it links only
    # cch_codemode_portable, so a portable subset that secretly pulls a
    # native-only dependency fails to link or fails the configure-time graph
    # check in cmake/targets/Codemode.cmake.
    add_executable(cch_tests_codemode_portable
        tests/Catch2Main.cpp
        tests/codemode/CodemodePortableTest.cpp
)
    target_include_directories(cch_tests_codemode_portable PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_codemode_portable
        PRIVATE
            cch_codemode_portable
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_codemode_portable PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_codemode_portable PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_codemode_portable ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_codemode_portable ${CCH_PARITY_BUILD_GATE_TARGET})
