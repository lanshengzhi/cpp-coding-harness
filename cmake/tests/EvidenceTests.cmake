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

    # #1058: the consolidated capability ledger is a generated 48k-line artifact, so its
    # deterministic rebuild, semantic checks and pointer-correction scoping are an ordinary
    # required test rather than a command a human remembers to run. The timeout bounds the
    # deterministic rebuild plus the frozen-pointer scan.
    add_test(
        NAME cch_pi_v130_consolidation_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
    )
    set_tests_properties(cch_pi_v130_consolidation_check PROPERTIES
        LABELS "evidence;consolidation;issue1058;spec"
        TIMEOUT 120)

    # #1058: boundary regressions for the consolidation tool. They separate the intended
    # correction from an already-qualified pointer, a different symbol on the same path and an
    # out-of-scope row, and they assert the derived orphan count and the product-row routing.
    add_test(
        NAME cch_pi_v130_consolidation_boundaries
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/evidence/ConsolidationAuditTest.py
    )
    set_tests_properties(cch_pi_v130_consolidation_boundaries PROPERTIES
        LABELS "evidence;consolidation;issue1058;spec"
        TIMEOUT 120)
