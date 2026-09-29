cmake_minimum_required(VERSION 4.4)

# Illegal Parity Architecture Gate fixture for the `cch_mcp` Owner Package
# (ADR 0065; issue #834). Identical to the legal fixture except that the
# `cch_tui` source is replaced by a poison source carrying one illegal include:
# the TUI Owner Package reaching into `cch_mcp`, which declares no legal Owner
# dependency. The Gate must reject this deterministically at configure time
# with rule ID PARITY-4007.
#
# Injected by the test harness:
#   CCH_PARITY_CMAKE_MODULE_PATH  directory containing ParityArchitecture.cmake
#   CCH_PARITY_MANIFEST           path to the strict versioned manifest
#   CCH_PARITY_FIXTURE_SRC        shared minimal fixture source tree
#   CCH_PARITY_FIXTURE_ROOT       fixture tree against which interface roots resolve
#   CCH_PARITY_POISON_SRC         poison source replacing cch_tui's source

project(parity_gate_fixture_illegal_mcp_edge LANGUAGES CXX)

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
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/ai/model.cpp
    DEPENDS cch_support
    INTERFACE_DEPENDS cch_support
)

cch_parity_declare_target(
    TARGET cch_mcp
    ROLE owner
    OWNER cch_mcp
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/mcp/host.cpp
    DEPENDS cch_support
)

cch_parity_declare_target(
    TARGET cch_tui
    ROLE owner
    OWNER cch_tui
    SOURCES ${CCH_PARITY_POISON_SRC}
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
    DEPENDS cch_agent_core cch_ai cch_mcp cch_support
)

cch_parity_declare_target(
    TARGET frontend_tui
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/coding_agent/tui/render.cpp
    DEPENDS cch_coding_agent cch_tui cch_support
)

cch_parity_declare_target(
    TARGET frontend_cli
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES ${CCH_PARITY_FIXTURE_SRC}/cli/startup.cpp
    DEPENDS cch_coding_agent frontend_tui cch_tui cch_support
)

cch_parity_run_gate("${CCH_PARITY_MANIFEST}" PROJECT_ROOT "${CCH_PARITY_FIXTURE_ROOT}")
