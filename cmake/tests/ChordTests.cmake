include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # chord
    add_executable(cch_tests_chord
        tests/Catch2Main.cpp
        tests/chord/ChordContextTest.cpp
        tests/chord/DeltaTest.cpp
        tests/chord/TrackerTest.cpp
        tests/chord/StrictJsonTest.cpp
        tests/chord/OutputTest.cpp
)
    target_include_directories(cch_tests_chord PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_chord
        PRIVATE
            cch_chord
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_chord PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_chord PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_chord ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_chord ${CCH_PARITY_BUILD_GATE_TARGET})
