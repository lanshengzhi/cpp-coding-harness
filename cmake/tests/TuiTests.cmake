include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cch_tui_named_evidence_boundary
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui/PiTuiEvidenceTest.py)
    set_tests_properties(cch_tui_named_evidence_boundary PROPERTIES
        LABELS "tui;differential;issue947;compat-pi")

    add_test(NAME cch_tui_capability_ledger
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui/TuiCapabilityLedgerTest.py)
    set_tests_properties(cch_tui_capability_ledger PROPERTIES
        LABELS "tui;differential;issue948;compat-pi")

    add_test(NAME cch_tui_utils_width_evidence
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui/TuiUtilsWidthEvidenceTest.py)
    set_tests_properties(cch_tui_utils_width_evidence PROPERTIES
        LABELS "tui;differential;issue956;compat-pi")

    add_test(NAME cch_tui_utils_ansi_evidence
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui/TuiUtilsAnsiEvidenceTest.py)
    set_tests_properties(cch_tui_utils_ansi_evidence PROPERTIES
        LABELS "tui;differential;issue957;compat-pi")

    # TUI
    add_executable(cch_tests_tui
        tests/Catch2Main.cpp
        tests/tui/AutocompleteTest.cpp
        tests/tui/ContainerTest.cpp
        tests/tui/EditorTest.cpp
        tests/tui/EditorLayoutTest.cpp
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
        tests/tui/StdinBufferTest.cpp
        tests/tui/TextBufferTest.cpp
        tests/tui/TruncatedTextTest.cpp
        tests/tui/TuiTest.cpp
        tests/tui/UnicodeWidthTest.cpp
        tests/tui/UtilsAnsiTest.cpp
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
        CCH_PYTHON3="${Python3_EXECUTABLE}"
)
    target_compile_options(cch_tests_tui PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_tui ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_tui ${CCH_PARITY_BUILD_GATE_TARGET})
