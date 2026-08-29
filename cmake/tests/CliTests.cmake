include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # CLI and architecture. The only shard that launches the built binary
    # (CliSmokeTest), so it carries the PIKE_EXECUTABLE definition and a build
    # dependency on pike.
    add_executable(cch_tests_cli_arch
        tests/Catch2Main.cpp
        tests/support/ScriptedProvider.cpp
        tests/support/ModelRuntimeTestSupport.cpp
        tests/cli/CliParseTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/cli/FrontendSelectionTest.cpp
        tests/cli/InitialPromptTest.cpp
        tests/cli/InstallRelocationTest.cpp
        tests/cli/ListModelsTest.cpp
        tests/cli/PrintModeTest.cpp
        tests/cli/SessionFamilyCliTest.cpp
        tests/cli/StartupTuiTest.cpp
        tests/architecture/MoveOnlyCallbackTest.cpp
        tests/architecture/PublicHeaderBoundaryTest.cpp
)
    target_include_directories(cch_tests_cli_arch PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_cli_arch
        PRIVATE
            cch_coding_agent
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_cli_arch PRIVATE
        PIKE_EXECUTABLE="$<TARGET_FILE:pike>"
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        CCH_BUILD_DIR="${CMAKE_BINARY_DIR}"
        CCH_CMAKE_COMMAND="${CMAKE_COMMAND}"
        CCH_PYTHON3="${Python3_EXECUTABLE}"
)
    target_compile_options(cch_tests_cli_arch PRIVATE ${CCH_WARNING_OPTIONS})
    add_dependencies(cch_tests_cli_arch pike)
    # Sanitizer builds link the sanitizer runtimes and are not installable:
    # the relocation test skips itself under them (issue #473).
    if(NOT CCH_SANITIZER STREQUAL "")
        target_compile_definitions(cch_tests_cli_arch PRIVATE CCH_SANITIZER_BUILD=1)
    endif()
    catch_discover_tests(cch_tests_cli_arch ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_cli_arch ${CCH_PARITY_BUILD_GATE_TARGET})
