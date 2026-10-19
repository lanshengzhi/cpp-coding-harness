include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

    # #1046: shared named evidence registry for the pi-v1.3.0 audit families (ADR 0075).
    # The test verifies the committed family, the fail-closed negative cases and, when a frozen
    # pi checkout with tsx is available, the capture-to-verified-read path.
    add_test(
        NAME cch_pi_v130_named_evidence
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/evidence/NamedEvidenceTest.py
    )
    set_tests_properties(cch_pi_v130_named_evidence PROPERTIES
        LABELS "evidence;differential;issue1046;spec")
