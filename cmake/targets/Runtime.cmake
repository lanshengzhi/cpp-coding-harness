include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# The executable is a thin closure over the repository-private CLI frontend:
# it compiles only the entry point and links the frontend adapter, which in
# turn composes the headless Session library with the optional TUI frontend.
cch_parity_declare_target(
    TARGET pike
    ROLE composition
    OWNER cch_coding_agent
    KIND executable
    SOURCES
        src/main.cpp
    DEPENDS
        frontend_cli
)
# main.cpp includes the private CLI runtime header through the repository
# source root. Its transitive TUI interface headers remain private to the
# composition target.
target_include_directories(pike PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tui/include
)

# Release distribution size optimization (issue #639): strip residual unwind
# tables from third-party static dependencies.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET pike POST_BUILD
        COMMAND "${CMAKE_STRIP}" --remove-section=.eh_frame "$<TARGET_FILE:pike>"
        COMMENT "Stripping .eh_frame from Release runtime executable"
    )
endif()
