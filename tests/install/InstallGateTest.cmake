# Runtime-only install gate fixture driver (issue #472).
#
# Drives the install-gate fixture (tests/fixtures/install-gate) end to end to
# prove the install path cannot bypass architecture validation:
#   1. configure succeeds (configure-phase Gate passes),
#   2. `cmake --install` before any build fails closed and stages no files,
#   3. after a build the install succeeds with only bin/ + licenses + the
#      codemode guest staged,
#   4. editing a source without rebuilding fails the install as stale,
#   5. rebuilding restores a passing install,
#   6. an object outside the Runtime's link closure never dates the binary, and
#      an object inside it still does (issue #1044).
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

# The install fixture uses the same miniature source tree as the architecture
# fixture, with independent generated state so their parallel runs cannot race.
include("${CCH_SOURCE_DIR}/tests/support/ParityFixture.cmake")
set(fixture_template "${fixture_root}")
set(fixture_root "${CCH_SOURCE_DIR}/build/install-parity-source")
cch_prepare_parity_fixture_source("${fixture_root}" "${fixture_template}")
set(fixture_src "${fixture_root}/src")

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

# 3. Build, then install: only the Runtime, the notice, and the codemode
# guest land in the prefix.
run_step(build TRUE "${CMAKE_COMMAND}" --build "${build_dir}")
run_step(install TRUE "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")
file(GLOB_RECURSE staged_files RELATIVE "${stage_dir}" "${stage_dir}/*")
list(SORT staged_files)
if(NOT staged_files STREQUAL
        "bin/pike;share/pike/codemode/quickjs.wasm;share/pike/licenses/fixture.txt")
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

# 6. The freshness check dates the Runtime binary against its own link closure
#    only (issue #1044). The fixture compiles cch_tui but `pike` does not link
#    it, so the two objects below separate the closure-correct comparison from
#    the whole-evidence-set comparison this step replaces: the old check failed
#    on either touch, the closure-correct one fails only on the linked object.
set(unlinked_object
    "${build_dir}/CMakeFiles/cch_tui.dir/${fixture_src}/tui/render.cpp.o")
set(linked_object
    "${build_dir}/CMakeFiles/cch_coding_agent.dir/${fixture_src}/coding_agent/compose.cpp.o")
foreach(object IN ITEMS "${unlinked_object}" "${linked_object}")
    if(NOT EXISTS "${object}")
        message(FATAL_ERROR "install gate fixture: expected compiled object missing: ${object}")
    endif()
endforeach()

# 6a. Touching an object outside the closure leaves the Runtime fresh: ninja
#     reports no work for the target, and the install passes.
file(TOUCH "${unlinked_object}")
run_step(ninja_after_unlinked_touch TRUE
    "${CMAKE_COMMAND}" --build "${build_dir}" --target pike -- -n)
if(NOT ninja_after_unlinked_touch_OUTPUT MATCHES "no work to do")
    message(FATAL_ERROR
        "touching an unlinked object made the Runtime target dirty:\n"
        "${ninja_after_unlinked_touch_OUTPUT}")
endif()
run_step(install_after_unlinked_touch TRUE
    "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")

# 6b. Negative control: touching an object inside the closure still fails the
#     install as a stale Runtime, naming that object.
file(TOUCH "${linked_object}")
run_step(install_after_linked_touch FALSE
    "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")
if(NOT install_after_linked_touch_OUTPUT MATCHES "stale Runtime binary")
    message(FATAL_ERROR
        "touching a linked object did not report a stale Runtime binary:\n"
        "${install_after_linked_touch_OUTPUT}")
endif()
if(NOT install_after_linked_touch_OUTPUT MATCHES "compose.cpp.o")
    message(FATAL_ERROR
        "the stale-Runtime diagnostic did not name the linked object:\n"
        "${install_after_linked_touch_OUTPUT}")
endif()

# 6c. Rebuilding the Runtime restores a passing install.
run_step(rebuild_after_linked_touch TRUE "${CMAKE_COMMAND}" --build "${build_dir}")
run_step(reinstall_after_linked_touch TRUE
    "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${stage_dir}")

message(STATUS "install gate fixture: PASS")
