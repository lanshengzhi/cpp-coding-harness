include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Central Parity Architecture constructors (ADR 0039). Every production target
# is declared through cch_parity_declare_target so its role, Owner, source
# ownership, direct dependencies, interface visibility, and external-family
# classification are recorded as configured evidence and emitted as the
# resolved ownership index. The manifest is the strict policy authority the
# Gate validates that evidence against.

cch_parity_set_manifest("${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json")

# Mandatory fail-closed Parity Architecture Gate at configure time (ADR 0039;
# issue #470). Configure validates the resolved ownership index and the
# direct-include evidence against the strict manifest for every supported
# configuration, including configurations with tests disabled; a rejection
# fails configuration with the validator's deterministic report. The
# build-phase Gate (compile commands and active depfile evidence) is wired in
# after all production targets are declared (cch_parity_add_build_gate below).
set(CCH_PARITY_EXTERNAL_INCLUDE_ROOTS
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
cch_parity_run_gate(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    EXTERNAL_INCLUDE_ROOTS ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
    STRICT_NO_EXCEPTIONS ${CCH_STRICT_NO_EXCEPTIONS}
)

# Mandatory build-phase Parity Architecture Gate (ADR 0039; issue #470). The
# `cch_parity_gate_build` target is a member of `all` and depends on every
# declared production target. A build-only barrier orders the composition
# executable after all other production targets, so its POST_BUILD Gate runs
# with stable `.ninja_deps` evidence. The declared ownership graph remains
# based on the explicit link dependencies. Test executables depend on
# ${CCH_PARITY_BUILD_GATE_TARGET} so every CTest entry point requires fresh
# successful Gate evidence.
cch_parity_add_build_gate(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    BUILD_DIR "${CMAKE_BINARY_DIR}"
    COMPOSITION_TARGET pike
    EXTERNAL_INCLUDE_ROOTS ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
    STRICT_NO_EXCEPTIONS ${CCH_STRICT_NO_EXCEPTIONS}
)
cch_parity_attach_build_gate_post_build(pike)
