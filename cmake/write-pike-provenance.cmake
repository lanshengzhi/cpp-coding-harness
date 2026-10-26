# Post-build provenance sidecar generator for the `pike` runtime executable.
#
# Invoked as:
#   cmake -DBINARY_PATH=<path-to-pike> -DSOURCE_DIR=<repo-root> -P cmake/write-pike-provenance.cmake
#
# Binds the built binary's post-link/post-strip SHA-256 to the source tree's
# Git HEAD and tracked dirty state without injecting volatile macros into C++
# translation units (preserving ccache hits and keeping `pike --version`
# output pi-aligned).

if(NOT DEFINED BINARY_PATH OR "${BINARY_PATH}" STREQUAL "")
    message(FATAL_ERROR "write-pike-provenance.cmake: BINARY_PATH is required")
endif()
if(NOT DEFINED SOURCE_DIR OR "${SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "write-pike-provenance.cmake: SOURCE_DIR is required")
endif()

file(SHA256 "${BINARY_PATH}" binary_sha256)
string(TIMESTAMP built_at "%Y-%m-%dT%H:%M:%SZ" UTC)

execute_process(
    COMMAND git -C "${SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE git_head_res
    OUTPUT_VARIABLE git_head
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(git_head_res EQUAL 0 AND NOT "${git_head}" STREQUAL "")
    execute_process(
        COMMAND git -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
        RESULT_VARIABLE git_status_res
        OUTPUT_VARIABLE git_tracked_status
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(git_status_res EQUAL 0)
        if("${git_tracked_status}" STREQUAL "")
            set(git_dirty "false")
            set(build_source_state "clean (${git_head})")
        else()
            set(git_dirty "true")
            set(build_source_state "dirty (${git_head})")
        endif()
    else()
        set(git_dirty "false")
        set(build_source_state "unknown")
    endif()
else()
    set(git_head "unknown")
    set(git_dirty "false")
    set(build_source_state "unknown")
endif()

set(provenance_path "${BINARY_PATH}.provenance.json")
set(tmp_path "${BINARY_PATH}.provenance.json.tmp")
file(WRITE "${tmp_path}"
"{\n"
"  \"binary_sha256\": \"${binary_sha256}\",\n"
"  \"git_head\": \"${git_head}\",\n"
"  \"git_dirty\": ${git_dirty},\n"
"  \"build_source_state\": \"${build_source_state}\",\n"
"  \"built_at\": \"${built_at}\"\n"
"}\n"
)
file(RENAME "${tmp_path}" "${provenance_path}")
