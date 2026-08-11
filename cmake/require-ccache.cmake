# Configure-time guard for the dev-fast presets.
#
# The dev-fast preset family is the checked-in Ninja + ccache fast-development
# path (docs/build-performance-plan.md, Stage 2). ccache is mandatory there:
# it is what makes warm rebuilds fast, and a missing launcher would otherwise
# fail only at build time with a generic "command not found". This script is
# wired into the dev-fast configure presets through CMAKE_PROJECT_INCLUDE so
# configure fails immediately, with a clear message, when ccache is absent.
#
# It also prints an explicit status line so cache use is never silent — a
# silent or nondeterministic cache is a Stage 2 No-Go signal.

find_program(CCACHE_PROGRAM NAMES ccache)

if(NOT CCACHE_PROGRAM)
  message(FATAL_ERROR
    "The dev-fast preset requires ccache, but ccache was not found on PATH.\n"
    "Install ccache (for example `sudo apt install ccache`, `sudo dnf install\n"
    "ccache`, or `brew install ccache`) and re-run the configure, or use a\n"
    "baseline preset instead (cmake --preset vcpkg)."
  )
endif()

message(STATUS "dev-fast: ccache enabled at ${CCACHE_PROGRAM}")
