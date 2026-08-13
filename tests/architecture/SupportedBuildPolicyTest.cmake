if(NOT DEFINED CCH_SOURCE_DIR)
    message(FATAL_ERROR "CCH_SOURCE_DIR is required")
endif()
if(NOT DEFINED CCH_TEST_CASE)
    message(FATAL_ERROR "CCH_TEST_CASE is required")
endif()
if(NOT DEFINED CCH_CASE_SHOULD_PASS)
    message(FATAL_ERROR "CCH_CASE_SHOULD_PASS is required")
endif()

set(case_script "${CCH_SOURCE_DIR}/tests/architecture/SupportedBuildCase.cmake")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DCCH_SOURCE_DIR=${CCH_SOURCE_DIR}"
        "-DCCH_TEST_CASE=${CCH_TEST_CASE}"
        -P "${case_script}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
set(combined "${output}\n${error}")

if(CCH_CASE_SHOULD_PASS AND NOT result EQUAL 0)
    message(FATAL_ERROR
        "Supported-build policy case '${CCH_TEST_CASE}' unexpectedly failed:\n${combined}")
endif()
if(NOT CCH_CASE_SHOULD_PASS AND result EQUAL 0)
    message(FATAL_ERROR
        "Supported-build policy case '${CCH_TEST_CASE}' unexpectedly passed")
endif()
if(NOT CCH_CASE_SHOULD_PASS AND NOT combined MATCHES "${CCH_DIAGNOSTIC_PATTERN}")
    message(FATAL_ERROR
        "Supported-build policy case '${CCH_TEST_CASE}' did not emit actionable diagnostic "
        "'${CCH_DIAGNOSTIC_PATTERN}':\n${combined}")
endif()
