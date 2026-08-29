include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    cch_add_supported_build_policy_test("accept-gcc" TRUE "")
    cch_add_supported_build_policy_test("accept-clang" TRUE "")
    cch_add_supported_build_policy_test("reject-cross-build" FALSE "Cross-compilation is unsupported")
    cch_add_supported_build_policy_test("reject-operating-system" FALSE "native Linux")
    cch_add_supported_build_policy_test("reject-architecture" FALSE "x86-64")
    cch_add_supported_build_policy_test("reject-libc" FALSE "glibc")
    cch_add_supported_build_policy_test("reject-generator" FALSE "Ninja generator")
    cch_add_supported_build_policy_test("reject-ninja-version" FALSE "Ninja 1.11 or newer")
    cch_add_supported_build_policy_test("reject-unity" FALSE "Unity Build")
    cch_add_supported_build_policy_test("reject-gcc-version" FALSE "GCC 16.x")
    cch_add_supported_build_policy_test("reject-clang-version" FALSE "Clang 22.x")
    cch_add_supported_build_policy_test("reject-clang-build-role" FALSE "build role requires GCC 16.x")
    cch_add_supported_build_policy_test("reject-clang-release" FALSE "requires a Debug configuration")
    cch_add_supported_build_policy_test("accept-sanitizer-address-undefined" TRUE "")
    cch_add_supported_build_policy_test("accept-sanitizer-thread" TRUE "")
    cch_add_supported_build_policy_test("reject-unknown-sanitizer" FALSE "Unknown CCH_SANITIZER entry")
    cch_add_supported_build_policy_test("reject-sanitizer-combination" FALSE "cannot be combined")
    cch_add_supported_build_policy_test("reject-duplicate-sanitizer" FALSE "Duplicate CCH_SANITIZER entry")
    cch_add_supported_build_policy_test("reject-dependency-fallback" FALSE "outside the pinned vcpkg prefix")
    cch_add_supported_build_policy_test("reject-missing-vcpkg-toolchain" FALSE "pinned vcpkg toolchain")
    cch_add_supported_build_policy_test("reject-vcpkg-manifest-install" FALSE "manifest installation is required")
    cch_add_supported_build_policy_test("reject-vcpkg-host-triplet" FALSE "host triplet")
    cch_add_supported_build_policy_test("reject-vcpkg-overlays" FALSE "overlay ports")

    # Parity Architecture Gate (ADR 0039). The Python validator uses only the
    # standard library and Python 3.12+; it is a system tool, not a vcpkg
    # dependency. The unit case exercises the strict manifest, the
    # direct-include lexer and canonical resolution, compile-context,
    # depfile-evidence, and evidence-producer rules directly; the fixture case
    # configures a minimal legal fixture, an illegal cross-Owner target-edge
    # fixture, and a set of poison-source fixtures end to end and asserts the
    # stable rule-ID rejections (PARITY-2001, PARITY-4xxx, PARITY-6001).
    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME cch_parity_gate_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/parity_gate_test.py
    )
    set_tests_properties(cch_parity_gate_unit PROPERTIES
        LABELS "architecture;parity-gate;issue448;issue449;issue470;issue480")

    add_test(
        NAME cch_warning_gate_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/WarningGateTest.py
    )
    set_tests_properties(cch_warning_gate_unit PROPERTIES
        LABELS "architecture;parity-gate;issue499")

    add_test(
        NAME cch_parity_gate_fixture
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/ParityGateTest.cmake
    )
    set_tests_properties(cch_parity_gate_fixture PROPERTIES
        LABELS "architecture;parity-gate;issue448;issue449;issue470;issue480")

    # Production build-phase Gate self-check (ADR 0039; issue #470): run the
    # same fail-closed build-phase Gate against the production evidence
    # (manifest, index, direct-include evidence, compile commands, and fresh
    # depfile evidence) and assert it passes. This proves the production
    # repository satisfies every manifest rule after package contraction and
    # that the deterministic machine-readable report is written. It runs after
    # the build has produced fresh depfiles; the build-phase Gate custom
    # target is already a prerequisite of every test shard.
    add_test(
        NAME cch_parity_gate_production_build
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_PARITY_GATE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/parity_gate.py
            -DCCH_PARITY_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json
            -DCCH_PARITY_INDEX=${CMAKE_BINARY_DIR}/parity-ownership-index.json
            -DCCH_PARITY_DIRECT_INCLUDES=${CMAKE_BINARY_DIR}/parity-direct-includes.json
            -DCCH_PARITY_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
            -DCCH_PARITY_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DCCH_PARITY_DEPFILES=${CMAKE_BINARY_DIR}/parity-build-gate-depfiles.json
            -DCCH_PARITY_REPORT=${CMAKE_BINARY_DIR}/parity-build-gate.json
            -DCCH_PARITY_EXTERNAL_INCLUDE_ROOTS=${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/run-build-gate.cmake
    )
    set_tests_properties(cch_parity_gate_production_build PROPERTIES
        LABELS "architecture;parity-gate;issue470;issue480")

    # Owner Interface standalone compile (ADR 0039; #469): every Owner
    # Interface header compiles alone with the include path restricted to its
    # declared package interface dependencies (own root, support, legal direct
    # Owner dependencies). No third-party or private root is provided, so
    # third-party leakage, private paths, and undeclared Owner edges fail.
    add_test(
        NAME cch_owner_interface_standalone
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/owner_interface_standalone.py
            --compiler ${CMAKE_CXX_COMPILER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json
            --project-root ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(cch_owner_interface_standalone PROPERTIES LABELS "architecture;parity-gate;issue469")

    # Zero-compiler-warning gate (issue #492): recompiles every project-owned
    # compile command from the generated compile_commands.json with the
    # project's own `-Wall -Wextra -Wpedantic` profile plus `-Werror` (and
    # `-fsyntax-only`), failing closed on any diagnostic in the production or
    # test source graph. CODING_STANDARDS.md §14 keeps compiler diagnostics
    # as review findings without warnings-as-errors in the normal build; this
    # gate makes the "zero warnings" requirement enforceable against
    # regressions. Requires the build to have produced compile_commands.json;
    # the build-phase Gate custom target is already a prerequisite of every
    # test shard.
    # Concurrency accounting: the gate's compile workers are a machine-wide
    # load, so size them to the host and reserve the same width from CTest
    # via PROCESSORS. Under a narrower `ctest -j N` the gate then runs
    # exclusively once all N slots drain instead of stacking its workers on
    # top of N sibling tests, keeping the host fully but never
    # oversubscribed. Host core count is safe to query: cross-compilation is
    # unsupported (ADR 0039).
    cmake_host_system_information(RESULT CCH_WARNING_GATE_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
    if(NOT CCH_WARNING_GATE_JOBS)
        set(CCH_WARNING_GATE_JOBS 4)
    endif()
    add_test(
        NAME cch_warning_gate
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/warning_gate.py
            --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
            --project-root ${CMAKE_CURRENT_SOURCE_DIR}
            --exclude-root ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
            --jobs ${CCH_WARNING_GATE_JOBS}
    )
    set_tests_properties(cch_warning_gate PROPERTIES
        LABELS "architecture;build;issue492"
        PROCESSORS ${CCH_WARNING_GATE_JOBS}
        TIMEOUT 900)
