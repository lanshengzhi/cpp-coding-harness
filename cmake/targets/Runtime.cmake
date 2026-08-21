include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# The executable is a thin closure over the repository-private
# cch_coding_agent library (#468): it compiles only the entry point and links
# the one authoritative Owner library.
cch_parity_declare_target(
    TARGET cpp_harness
    ROLE composition
    OWNER cch_coding_agent
    KIND executable
    SOURCES
        src/main.cpp
    DEPENDS
        cch_coding_agent
)
# main.cpp compiles against the Owner's private runtime headers (the src
# root); those headers reference the cch_tui interface, which is a legal
# private cch_coding_agent edge and therefore not interface-published, so the
# composition target adds the tui interface root privately.
target_include_directories(cpp_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tui/include
)
