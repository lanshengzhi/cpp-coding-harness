cmake_minimum_required(VERSION 4.4)

# Legal Parity Architecture Gate fixture. Declares the minimal Owner graph from
# the strict manifest (cmake/parity/manifest.json) with only legal cross-Owner
# target edges and only canonical project-header includes, then runs the
# fail-closed Gate at configure time. This fixture must configure successfully.
#
# Injected by the test harness:
#   CCH_PARITY_CMAKE_MODULE_PATH  directory containing ParityArchitecture.cmake
#   CCH_PARITY_MANIFEST           path to the strict versioned manifest
#   CCH_PARITY_FIXTURE_SRC        shared minimal fixture source tree
#   CCH_PARITY_FIXTURE_ROOT       fixture tree against which interface roots resolve

project(parity_gate_fixture_legal LANGUAGES CXX)

foreach(required CCH_PARITY_CMAKE_MODULE_PATH CCH_PARITY_MANIFEST CCH_PARITY_FIXTURE_SRC CCH_PARITY_FIXTURE_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

# Compile commands are generated evidence: the post-generation Gate validates
# them (see tests/architecture/ParityGateTest.cmake).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(PREPEND CMAKE_MODULE_PATH "${CCH_PARITY_CMAKE_MODULE_PATH}")
include(ParityArchitecture)

cch_parity_set_manifest("${CCH_PARITY_MANIFEST}")

cch_parity_declare_target(
    TARGET cch_support
    ROLE support
    OWNER cch_support
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/support/value.cpp
)

cch_parity_declare_target(
    TARGET cch_ai
    ROLE owner
    OWNER cch_ai
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/ai/model.cpp
    DEPENDS cch_support
    INTERFACE_DEPENDS cch_support
    FORCED_INCLUDES ${CCH_PARITY_FIXTURE_ROOT}/src/ai/include/cch/ai/force.hpp
    PCH_INPUT ${CCH_PARITY_FIXTURE_ROOT}/src/ai/include/cch/ai/pch.hpp
)

cch_parity_declare_target(
    TARGET cch_tui
    ROLE owner
    OWNER cch_tui
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/tui/render.cpp
    DEPENDS cch_support
)

cch_parity_declare_target(
    TARGET cch_agent_core
    ROLE owner
    OWNER cch_agent_core
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/agent_core/agent.cpp
    DEPENDS cch_ai cch_support
)

cch_parity_declare_target(
    TARGET cch_coding_agent
    ROLE owner
    OWNER cch_coding_agent
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/coding_agent/compose.cpp
    DEPENDS cch_agent_core cch_ai cch_tui cch_support
)

cch_parity_run_gate("${CCH_PARITY_MANIFEST}" PROJECT_ROOT "${CCH_PARITY_FIXTURE_ROOT}")
