cmake_minimum_required(VERSION 4.4)

# Runtime-only install fixture (issue #472). Declares the minimal legal Owner
# graph from the strict manifest (like the parity-gate legal fixture), adds one
# composition executable, and wires the real Runtime install rules so the
# driver (tests/install/InstallGateTest.cmake) can prove the install path
# requires fresh successful Gate evidence:
#   - `cmake --install` before any build fails closed (no depfile evidence),
#   - after a build it installs only bin/ + licenses,
#   - editing a source without rebuilding fails the install as stale.
#
# Injected by the test harness:
#   CCH_PARITY_CMAKE_MODULE_PATH  directory containing ParityArchitecture.cmake
#                                 and RuntimeInstall.cmake
#   CCH_PARITY_MANIFEST           path to the strict versioned manifest
#   CCH_PARITY_FIXTURE_SRC        shared minimal fixture source tree
#   CCH_PARITY_FIXTURE_ROOT       fixture tree against which interface roots resolve

project(install_gate_fixture LANGUAGES CXX)

foreach(required CCH_PARITY_CMAKE_MODULE_PATH CCH_PARITY_MANIFEST CCH_PARITY_FIXTURE_SRC CCH_PARITY_FIXTURE_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

# Compile commands are generated evidence: the install gate validates them.
# C++23 matches the production compile pipeline (module dependency scanning,
# persistent .ddi.d depfiles) so the build-phase Gate sees the same evidence
# shape as the production build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(PREPEND CMAKE_MODULE_PATH "${CCH_PARITY_CMAKE_MODULE_PATH}")
include(ParityArchitecture)
include(RuntimeInstall)

cch_parity_set_manifest("${CCH_PARITY_MANIFEST}")

cch_parity_declare_target(
    TARGET cch_support
    ROLE support
    OWNER cch_support
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/support/value.cpp
)

# Unlike the parity-gate fixtures this fixture really compiles and links, so
# each Owner publishes its fixture interface root; the constructor's link
# edges propagate those include directories to legal dependents.
target_include_directories(cch_support PUBLIC ${CCH_PARITY_FIXTURE_SRC}/support/include)

cch_parity_declare_target(
    TARGET cch_ai
    ROLE owner
    OWNER cch_ai
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/ai/model.cpp
    DEPENDS cch_support
    INTERFACE_DEPENDS cch_support
)
target_include_directories(cch_ai PUBLIC ${CCH_PARITY_FIXTURE_SRC}/ai/include)

cch_parity_declare_target(
    TARGET cch_tui
    ROLE owner
    OWNER cch_tui
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/tui/render.cpp
    DEPENDS cch_support
)
target_include_directories(cch_tui PUBLIC ${CCH_PARITY_FIXTURE_SRC}/tui/include)

cch_parity_declare_target(
    TARGET cch_agent_core
    ROLE owner
    OWNER cch_agent_core
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/agent_core/agent.cpp
    DEPENDS cch_ai cch_support
)
target_include_directories(cch_agent_core PUBLIC ${CCH_PARITY_FIXTURE_SRC}/agent/include)

cch_parity_declare_target(
    TARGET cch_coding_agent
    ROLE owner
    OWNER cch_coding_agent
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/coding_agent/compose.cpp
    DEPENDS cch_agent_core cch_ai cch_tui cch_support
)
target_include_directories(cch_coding_agent PUBLIC ${CCH_PARITY_FIXTURE_SRC}/coding_agent/include)

cch_parity_declare_target(
    TARGET pike
    ROLE composition
    OWNER cch_coding_agent
    KIND executable
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    DEPENDS cch_coding_agent
)

cch_parity_run_gate("${CCH_PARITY_MANIFEST}" PROJECT_ROOT "${CCH_PARITY_FIXTURE_ROOT}")

# The build-phase Gate produces the fresh depfile/report evidence the install
# gate requires.
cch_parity_add_build_gate(
    "${CCH_PARITY_MANIFEST}"
    PROJECT_ROOT "${CCH_PARITY_FIXTURE_ROOT}"
    BUILD_DIR "${CMAKE_BINARY_DIR}"
)

cch_runtime_install_rules(
    TARGET pike
    MANIFEST "${CCH_PARITY_MANIFEST}"
    PROJECT_ROOT "${CCH_PARITY_FIXTURE_ROOT}"
    LICENSE_FILES "fixture=${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.txt"
)
