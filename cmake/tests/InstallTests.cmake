include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # Runtime-only install (ADR 0039; issue #472). The unit case covers the
    # dependency-closure audit and the Gate-evidence freshness check with
    # synthetic fixtures; the fixture case drives a minimal project through
    # the real install rules to prove the install path fails closed without
    # fresh successful Gate evidence, stages only bin/ + licenses, and
    # rejects a stale (edited-but-not-rebuilt) tree.
    add_test(
        NAME cch_install_tools_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/install_tools_test.py
    )
    set_tests_properties(cch_install_tools_unit PROPERTIES LABELS "install;issue472")

    add_test(
        NAME cch_install_gate_fixture
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            "-DCCH_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/InstallGateTest.cmake
    )
    set_tests_properties(cch_install_gate_fixture PROPERTIES LABELS "install;issue472")

    # Release qualification evidence (ADR 0039; issue #474). The fail-closed
    # verifier checks the artifact lane's evidence directory: presence,
    # closed vocabularies, freshness against the run window, toolchain
    # contradiction rules, required PASS markers, and the artifact digest
    # binding; missing, stale, contradictory, or incomplete evidence fails
    # qualification rather than degrading to a warning.
    add_test(
        NAME cch_release_evidence_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/release_evidence_test.py
    )
    set_tests_properties(cch_release_evidence_unit PROPERTIES LABELS "install;release;issue474")
