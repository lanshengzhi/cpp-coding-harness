if(NOT DEFINED CCH_SOURCE_DIR)
    message(FATAL_ERROR "CCH_SOURCE_DIR is required")
endif()

# End-to-end Parity Architecture Gate fixture cases. Configure a minimal legal
# fixture (must succeed), an otherwise-equivalent illegal fixture carrying
# exactly one illegal cross-Owner target edge (PARITY-2001), and a set of
# poison-source fixtures each carrying exactly one rejected project-header
# spelling (PARITY-4xxx). After generation, the legal fixture's compile
# commands are validated and a build-phase Gate is run without depfile
# evidence to prove the active-dependency evidence requirement fails closed
# (PARITY-6001). The manifest and validator are the checked-in cmake/parity/*
# files, never per-test copies.

set(CCH_PARITY_CMAKE_MODULE_PATH "${CCH_SOURCE_DIR}/cmake")
set(CCH_PARITY_MANIFEST "${CCH_SOURCE_DIR}/cmake/parity/manifest.json")
set(CCH_PARITY_GATE_SCRIPT "${CCH_SOURCE_DIR}/cmake/parity/parity_gate.py")
set(CCH_PARITY_FIXTURE_ROOT "${CCH_SOURCE_DIR}/tests/fixtures/parity-gate")
set(CCH_PARITY_FIXTURE_SRC "${CCH_PARITY_FIXTURE_ROOT}/src")
set(CCH_PARITY_LEGAL_FIXTURE "${CCH_PARITY_FIXTURE_ROOT}/legal")
set(CCH_PARITY_ILLEGAL_FIXTURE "${CCH_PARITY_FIXTURE_ROOT}/illegal")
set(CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE "${CCH_PARITY_FIXTURE_ROOT}/illegal-include")
set(CCH_PARITY_POISON_DIR "${CCH_PARITY_FIXTURE_ROOT}/poison")
set(CCH_PARITY_AI_INTERFACE_ROOT "${CCH_PARITY_FIXTURE_ROOT}/src/ai/include/cch/ai")
set(CCH_PARITY_AI_PRIVATE_PROVIDER "${CCH_PARITY_FIXTURE_ROOT}/src/ai/providers/Provider.hpp")

# The fixture mirrors the real AI package boundary: Provider is a private
# capability, not an Owner Interface header. Keep both sides explicit so a
# future fixture update cannot silently put it back under the canonical root.
if(EXISTS "${CCH_PARITY_AI_INTERFACE_ROOT}/Provider.hpp")
    message(FATAL_ERROR "the parity fixture must not expose Provider.hpp from the cch_ai interface root")
endif()
if(NOT EXISTS "${CCH_PARITY_AI_PRIVATE_PROVIDER}")
    message(FATAL_ERROR "the parity fixture must keep Provider.hpp under the private AI root")
endif()

find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

function(cch_parity_configure_case fixture_dir build_dir should_pass diagnostic_pattern case_name)
    # Optional 6th argument names a poison source injected as CCH_PARITY_POISON_SRC.
    set(poison_arg "")
    if(ARGC GREATER 5)
        set(poison_arg "-DCCH_PARITY_POISON_SRC=${ARGV5}")
    endif()
    file(REMOVE_RECURSE "${build_dir}")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            -S "${fixture_dir}"
            -B "${build_dir}"
            -G Ninja
            "-DCCH_PARITY_CMAKE_MODULE_PATH=${CCH_PARITY_CMAKE_MODULE_PATH}"
            "-DCCH_PARITY_MANIFEST=${CCH_PARITY_MANIFEST}"
            "-DCCH_PARITY_FIXTURE_SRC=${CCH_PARITY_FIXTURE_SRC}"
            "-DCCH_PARITY_FIXTURE_ROOT=${CCH_PARITY_FIXTURE_ROOT}"
            ${poison_arg}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    set(combined "${output}\n${error}")

    if(should_pass AND NOT result EQUAL 0)
        message(FATAL_ERROR
            "Parity Gate fixture '${case_name}' unexpectedly failed to configure:\n${combined}")
    endif()
    if(NOT should_pass AND result EQUAL 0)
        message(FATAL_ERROR "Parity Gate fixture '${case_name}' unexpectedly configured successfully")
    endif()
    if(NOT should_pass AND NOT combined MATCHES "${diagnostic_pattern}")
        message(FATAL_ERROR
            "Parity Gate fixture '${case_name}' did not emit stable diagnostic "
            "'${diagnostic_pattern}':\n${combined}")
    endif()
endfunction()

cch_parity_configure_case(
    "${CCH_PARITY_LEGAL_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/legal"
    TRUE
    ""
    "legal"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/illegal"
    FALSE
    "PARITY-2001"
    "illegal"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/quote-spelling"
    FALSE
    "PARITY-4001"
    "quote-spelling"
    "${CCH_PARITY_POISON_DIR}/quote-spelling.cpp"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/unclassified-root"
    FALSE
    "PARITY-4002"
    "unclassified-root"
    "${CCH_PARITY_POISON_DIR}/unclassified-root.cpp"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/path-escape"
    FALSE
    "PARITY-4003"
    "path-escape"
    "${CCH_PARITY_POISON_DIR}/path-escape.cpp"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/case-conflict"
    FALSE
    "PARITY-4004"
    "case-conflict"
    "${CCH_PARITY_POISON_DIR}/case-conflict.cpp"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/macro-generated"
    FALSE
    "PARITY-4006"
    "macro-generated"
    "${CCH_PARITY_POISON_DIR}/macro-generated.cpp"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/illegal-direct-include"
    FALSE
    "PARITY-4007"
    "illegal-direct-include"
    "${CCH_PARITY_POISON_DIR}/illegal-direct-include.cpp"
)

cch_parity_configure_case(
    "${CCH_PARITY_ILLEGAL_INCLUDE_FIXTURE}"
    "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/unresolved"
    FALSE
    "PARITY-4008"
    "unresolved"
    "${CCH_PARITY_POISON_DIR}/unresolved.cpp"
)

# Post-generation Gate phase for the legal fixture: the generated compile
# commands must validate cleanly against the index and direct-include evidence.
set(legal_build_dir "${CCH_SOURCE_DIR}/build/parity-gate-fixtures/legal")

# The legal fixture declares cch_ai -> cch_support as interface-visible
# (INTERFACE_DEPENDS), so the emitted ownership index must record that
# dependency's visibility as "public" (the constructor's other dependencies
# default to "private").
file(READ "${legal_build_dir}/parity-ownership-index.json" legal_index_text)
string(FIND "${legal_index_text}"
    "\"name\":\"cch_support\",\"family\":null,\"visibility\":\"public\""
    public_visibility_position)
if(public_visibility_position EQUAL -1)
    message(FATAL_ERROR
        "legal fixture index did not record cch_ai -> cch_support as interface-visible (public)")
endif()
file(READ "${legal_build_dir}/parity-direct-includes.json" legal_direct_includes_text)
if(NOT legal_direct_includes_text MATCHES "\\\"path\\\"[ \\t]*:[ \\t]*\\\"ai/providers/Provider\\.hpp\\\"")
    message(FATAL_ERROR
        "legal fixture direct-include evidence did not retain the private Provider header")
endif()
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
        --manifest "${CCH_PARITY_MANIFEST}"
        --index "${legal_build_dir}/parity-ownership-index.json"
        --direct-includes "${legal_build_dir}/parity-direct-includes.json"
        --compile-commands "${legal_build_dir}/compile_commands.json"
        --project-root "${CCH_PARITY_FIXTURE_ROOT}"
        --format json
    RESULT_VARIABLE post_result
    OUTPUT_VARIABLE post_output
    ERROR_VARIABLE post_error
)
if(NOT post_result EQUAL 0)
    message(FATAL_ERROR
        "Parity Gate post-generation phase unexpectedly rejected the legal fixture:\n"
        "${post_output}${post_error}")
endif()

# Build-phase Gate: active dependency evidence (depfiles) is required and
# missing evidence must fail closed with PARITY-6001.
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
        --manifest "${CCH_PARITY_MANIFEST}"
        --index "${legal_build_dir}/parity-ownership-index.json"
        --direct-includes "${legal_build_dir}/parity-direct-includes.json"
        --compile-commands "${legal_build_dir}/compile_commands.json"
        --project-root "${CCH_PARITY_FIXTURE_ROOT}"
        --phase build
        --format json
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(build_result EQUAL 0)
    message(FATAL_ERROR "Parity Gate build phase unexpectedly passed without depfile evidence")
endif()
set(build_combined "${build_output}\n${build_error}")
if(NOT build_combined MATCHES "PARITY-6001")
    message(FATAL_ERROR
        "Parity Gate build phase did not emit PARITY-6001 for missing depfile evidence:\n"
        "${build_combined}")
endif()
