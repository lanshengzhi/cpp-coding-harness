include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # TUI
    add_executable(cch_tests_tui
        tests/Catch2Main.cpp
        tests/tui/AutocompleteTest.cpp
        tests/tui/ContainerTest.cpp
        tests/tui/EditorTest.cpp
        tests/tui/FuzzyTest.cpp
        tests/tui/ImageTest.cpp
        tests/tui/InputTest.cpp
        tests/tui/KeybindingsTest.cpp
        tests/tui/KeysTest.cpp
        tests/tui/LoaderTest.cpp
        tests/tui/MarkdownTest.cpp
        tests/tui/OverlayTest.cpp
        tests/tui/OverlayCompositorTest.cpp
        tests/tui/PiTuiDifferentialTest.cpp
        tests/tui/ProcessTerminalTest.cpp
        tests/tui/RenderDifferentialTest.cpp
        tests/tui/ScreenStateGoldenTest.cpp
        tests/tui/SelectListTest.cpp
        tests/tui/SettingsListTest.cpp
        tests/tui/TerminalImageTest.cpp
        tests/tui/TerminalStreamDecoderTest.cpp
        tests/tui/TextBufferTest.cpp
        tests/tui/TruncatedTextTest.cpp
        tests/tui/TuiTest.cpp
        tests/tui/UnicodeWidthTest.cpp
        tests/tui/UtilsTest.cpp
        tests/tui/VirtualTerminalTest.cpp
)
    target_include_directories(cch_tests_tui PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_tui
        PRIVATE
            cch_tui
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_tui PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_tui PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_tui ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_tui ${CCH_PARITY_BUILD_GATE_TARGET})
