# #1232 symbol ownership check: the test executable must link exactly one
# deterministic Remote implementation (the hook-enabled test variant), and the
# production codemode archive must remain free of the test-only worker gate.
# Invoked by `codemode_remote_symbol_ownership` with CMAKE_NM, ARCHIVE and
# BINARY supplied by the Codemode test shard.

if(NOT DEFINED CMAKE_NM OR NOT DEFINED ARCHIVE OR NOT DEFINED BINARY)
    message(FATAL_ERROR "RemoteProductionSymbols.cmake requires CMAKE_NM, ARCHIVE and BINARY")
endif()

function(cch_nm_defined output path)
    execute_process(
        COMMAND "${CMAKE_NM}" -C --defined-only "${path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE symbols
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "nm failed for '${path}': ${error}")
    endif()
    set(${output} "${symbols}" PARENT_SCOPE)
endfunction()

cch_nm_defined(archive_symbols "${ARCHIVE}")
if(archive_symbols MATCHES
        "DeterministicWorkerTestGate|park_deterministic_worker|record_deterministic_worker|deterministic_worker_test_gate")
    message(FATAL_ERROR "production codemode archive carries a deterministic worker test gate symbol")
endif()

cch_nm_defined(binary_symbols "${BINARY}")
string(REPLACE "\n" ";" binary_lines "${binary_symbols}")
list(FILTER binary_lines INCLUDE REGEX "[ \t]T[ \t]+cch::codemode::serve_codemode_remote\\(")
list(LENGTH binary_lines serve_count)
if(NOT serve_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one serve_codemode_remote definition in '${BINARY}', "
        "found ${serve_count}:\n${binary_lines}")
endif()
if(NOT binary_symbols MATCHES "deterministic_worker_test_gate")
    message(FATAL_ERROR "test executable did not link the hook-enabled codemode variant")
endif()
if(NOT binary_symbols MATCHES "RemoteDeterministicServe")
    message(FATAL_ERROR "test executable has no RemoteDeterministicServe implementation")
endif()

message(STATUS
    "Remote symbol ownership: one serve_codemode_remote, hook-enabled variant linked, "
    "production archive hook-free")
