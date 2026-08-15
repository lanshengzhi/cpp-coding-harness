# Build-phase Parity Architecture Gate runner (ADR 0039; issue #470).
#
# Invoked by the `cch_parity_gate_build` custom target, by the composition
# executable's POST_BUILD step, and by the production build-gate CTest case.
# It records the active dependency (depfile) evidence from the generated
# compile commands and validates the full evidence set against the strict
# manifest at the build phase; a rejected graph fails the invoking build or
# test with a deterministic machine-readable report.
#
# Required -D variables:
#   CCH_PARITY_GATE_SCRIPT       path to cmake/parity/parity_gate.py
#   CCH_PARITY_MANIFEST          path to cmake/parity/manifest.json
#   CCH_PARITY_INDEX             resolved ownership index (parity-ownership-index.json)
#   CCH_PARITY_DIRECT_INCLUDES   direct-include lexer evidence (parity-direct-includes.json)
#   CCH_PARITY_COMPILE_COMMANDS  generated compile_commands.json
#   CCH_PARITY_PROJECT_ROOT      project root against which roots resolve
#   CCH_PARITY_DEPFILES          output path for the recorded depfile evidence
#   CCH_PARITY_REPORT            output path for the machine-readable gate report
#
# Optional:
#   CCH_PARITY_EXTERNAL_INCLUDE_ROOTS  ';'-list of external dependency include
#                                      roots (for example the vcpkg installed
#                                      include directory); paths under them are
#                                      never project paths.

if(NOT DEFINED CCH_PARITY_GATE_SCRIPT OR
   NOT DEFINED CCH_PARITY_MANIFEST OR
   NOT DEFINED CCH_PARITY_INDEX OR
   NOT DEFINED CCH_PARITY_DIRECT_INCLUDES OR
   NOT DEFINED CCH_PARITY_COMPILE_COMMANDS OR
   NOT DEFINED CCH_PARITY_PROJECT_ROOT OR
   NOT DEFINED CCH_PARITY_DEPFILES OR
   NOT DEFINED CCH_PARITY_REPORT)
    message(FATAL_ERROR "run-build-gate.cmake: required -D variables are missing")
endif()

find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

# 1. Record the active dependency (depfile) evidence. The producer derives the
#    compiler depfile from each compile command; a compiled source whose
#    depfile does not exist is omitted and the validator fails closed on the
#    missing entry, so no active dependency evidence can disappear silently.
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
        --record-depfiles "${CCH_PARITY_DEPFILES}"
        --manifest "${CCH_PARITY_MANIFEST}"
        --index "${CCH_PARITY_INDEX}"
        --compile-commands "${CCH_PARITY_COMPILE_COMMANDS}"
    RESULT_VARIABLE record_result
    OUTPUT_VARIABLE record_output
    ERROR_VARIABLE record_error
)
if(NOT record_result EQUAL 0)
    message(FATAL_ERROR
        "Parity Architecture Gate could not record depfile evidence:\n"
        "${record_output}${record_error}")
endif()

# 2. Validate every evidence class at the build phase: configured target
#    evidence, source/include evidence, and active dependency evidence must be
#    mutually consistent; stale or missing evidence fails.
set(gate_command
    "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
    --manifest "${CCH_PARITY_MANIFEST}"
    --index "${CCH_PARITY_INDEX}"
    --direct-includes "${CCH_PARITY_DIRECT_INCLUDES}"
    --compile-commands "${CCH_PARITY_COMPILE_COMMANDS}"
    --depfiles "${CCH_PARITY_DEPFILES}"
    --project-root "${CCH_PARITY_PROJECT_ROOT}"
    --phase build
    --format json
)
if(DEFINED CCH_PARITY_EXTERNAL_INCLUDE_ROOTS AND NOT "${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}" STREQUAL "")
    foreach(external_root IN LISTS CCH_PARITY_EXTERNAL_INCLUDE_ROOTS)
        list(APPEND gate_command --external-include-root "${external_root}")
    endforeach()
endif()
execute_process(
    COMMAND ${gate_command}
    RESULT_VARIABLE gate_result
    OUTPUT_VARIABLE gate_output
    ERROR_VARIABLE gate_error
)

# The machine-readable report is written on every run, pass or fail, so
# automation always has the latest deterministic JSON diagnostics.
if(gate_output STREQUAL "")
    file(WRITE "${CCH_PARITY_REPORT}" "${gate_error}")
else()
    file(WRITE "${CCH_PARITY_REPORT}" "${gate_output}")
endif()

if(NOT gate_result EQUAL 0)
    message(FATAL_ERROR
        "Parity Architecture Gate (build phase) rejected the configured graph:\n"
        "${gate_output}${gate_error}")
endif()
message(STATUS "Parity Architecture Gate (build phase): PASS (see ${CCH_PARITY_REPORT})")
