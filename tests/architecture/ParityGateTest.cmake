if(NOT DEFINED CCH_SOURCE_DIR)
    message(FATAL_ERROR "CCH_SOURCE_DIR is required")
endif()

# End-to-end Parity Architecture Gate fixture cases: configure a minimal legal
# fixture (must succeed) and an otherwise-equivalent illegal fixture carrying
# exactly one illegal cross-Owner edge (must be rejected deterministically with
# rule ID PARITY-2001). The manifest and validator are the checked-in
# cmake/parity/* files, never per-test copies.

set(CCH_PARITY_CMAKE_MODULE_PATH "${CCH_SOURCE_DIR}/cmake")
set(CCH_PARITY_MANIFEST "${CCH_SOURCE_DIR}/cmake/parity/manifest.json")
set(CCH_PARITY_FIXTURE_SRC "${CCH_SOURCE_DIR}/tests/fixtures/parity-gate/src")
set(CCH_PARITY_LEGAL_FIXTURE "${CCH_SOURCE_DIR}/tests/fixtures/parity-gate/legal")
set(CCH_PARITY_ILLEGAL_FIXTURE "${CCH_SOURCE_DIR}/tests/fixtures/parity-gate/illegal")

function(cch_parity_configure_case fixture_dir build_dir should_pass diagnostic_pattern case_name)
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
