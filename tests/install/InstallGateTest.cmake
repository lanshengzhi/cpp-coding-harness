# Runtime-only install gate fixture driver (issue #472).
#
# Drives the install-gate fixture (tests/fixtures/install-gate) end to end to
# prove the install path cannot bypass architecture validation:
#   1. configure succeeds (configure-phase Gate passes),
#   2. `cmake --install` before any build fails closed and stages no files,
#   3. after a build the install succeeds with only bin/ + licenses staged,
#   4. editing a source without rebuilding fails the install as stale,
#   5. rebuilding restores a passing install.
#
# Required -D variables:
#   CCH_SOURCE_DIR     repository root
#   CCH_CXX_COMPILER   the main build's compiler (fixture builds gate-consistent)

if(NOT DEFINED CCH_SOURCE_DIR OR NOT DEFINED CCH_CXX_COMPILER)
    message(FATAL_ERROR "InstallGateTest.cmake: CCH_SOURCE_DIR and CCH_CXX_COMPILER are required")
endif()

set(fixture_dir "${CCH_SOURCE_DIR}/tests/fixtures/install-gate")
set(fixture_src "${CCH_SOURCE_DIR}/tests/fixtures/parity-gate/src")
set(fixture_root "${CCH_SOURCE_DIR}/tests/fixtures/parity-gate")
set(manifest "${CCH_SOURCE_DIR}/cmake/parity/manifest.json")
set(module_path "${CCH_SOURCE_DIR}/cmake")
set(work_dir "${CCH_SOURCE_DIR}/build/install-gate-fixture")
set(fixture_copy "${work_dir}/src")
set(build_dir "${work_dir}/build")
set(stage_dir "${work_dir}/stage")

function(run_step step_name expect_success)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    set(combined "${output}\n${error}")
    set(${step_name}_OUTPUT "${combined}" PARENT_SCOPE)
    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "${step_name} unexpectedly failed:\n${combined}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "${step_name} unexpectedly succeeded:\n${combined}")
    endif()
endfunction()

# The driver works on a private copy of the checked-in fixture so the staleness
# probe (step 4) touches only test-owned state, never the repository tree.
file(REMOVE_RECURSE "${work_dir}")
file(COPY "${fixture_dir}/" DESTINATION "${fixture_copy}")

# 1. Configure.
run_step(configure TRUE
    "${CMAKE_COMMAND}"
    -S "${fixture_copy}"
    -B "${build_dir}"
    -G Ninja
    "-DCMAKE_CXX_COMPILER=${CCH_CXX_COMPILER}"
    "-DCCH_PARITY_CMAKE_MODULE_PATH=${module_path}"
    "-DCCH_PARITY_MANIFEST=${manifest}"
    "-DCCH_PARITY_FIXTURE_SRC=${fixture_src}"
    "-DCCH_PARITY_FIXTURE_ROOT=${fixture_root}"
)

# 2. Install without a build fails closed and stages no files.
run_step(install_without_build FALSE
    "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")
if(NOT install_without_build_OUTPUT MATCHES "Gate")
    message(FATAL_ERROR
        "install without build did not fail through the Gate:\n${install_without_build_OUTPUT}")
endif()
file(GLOB_RECURSE staged_after_rejection "${stage_dir}/*")
if(staged_after_rejection)
    message(FATAL_ERROR
        "a rejected install staged files: ${staged_after_rejection}")
endif()

# 3. Build, then install: only the Runtime and the notice land in the prefix.
run_step(build TRUE "${CMAKE_COMMAND}" --build "${build_dir}")
run_step(install TRUE "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")
file(GLOB_RECURSE staged_files RELATIVE "${stage_dir}" "${stage_dir}/*")
list(SORT staged_files)
if(NOT staged_files STREQUAL "bin/pike;share/pike/licenses/fixture.txt")
    message(FATAL_ERROR
        "fixture install staged an unexpected file set: ${staged_files}")
endif()

# 4. A source edit without a rebuild makes the evidence stale: install fails.
file(TOUCH "${fixture_copy}/main.cpp")
run_step(install_stale FALSE
    "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")
if(NOT install_stale_OUTPUT MATCHES "stale build products")
    message(FATAL_ERROR
        "stale install did not report stale build products:\n${install_stale_OUTPUT}")
endif()

# 5. Rebuilding restores a passing install.
run_step(rebuild TRUE "${CMAKE_COMMAND}" --build "${build_dir}")
run_step(reinstall TRUE "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")

message(STATUS "install gate fixture: PASS")
