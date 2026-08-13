cmake_minimum_required(VERSION 4.4)

# Illegal direct-include Parity Architecture Gate fixture. Identical to the
# legal fixture except that `cch_ai`'s source is replaced by a poison source
# carrying exactly one rejected project-header spelling. The Gate must reject
# this deterministically at configure time with the rule ID asserted by the
# test harness (PARITY-4001/4002/4003/4004/4006/4007/4008).
#
# Injected by the test harness:
#   CCH_PARITY_CMAKE_MODULE_PATH  directory containing ParityArchitecture.cmake
#   CCH_PARITY_MANIFEST           path to the strict versioned manifest
#   CCH_PARITY_FIXTURE_SRC        shared minimal fixture source tree
#   CCH_PARITY_FIXTURE_ROOT       fixture tree against which interface roots resolve
#   CCH_PARITY_POISON_SRC         poison source replacing cch_ai's source

project(parity_gate_fixture_illegal_include LANGUAGES CXX)

foreach(required CCH_PARITY_CMAKE_MODULE_PATH CCH_PARITY_MANIFEST CCH_PARITY_FIXTURE_SRC CCH_PARITY_FIXTURE_ROOT CCH_PARITY_POISON_SRC)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

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
    SOURCES ${CCH_PARITY_POISON_SRC}
    DEPENDS cch_support
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
