include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Owner-local include roots (ADR 0039; #469). Each package publishes only its
# own interface root (<package>/include, which provides the canonical
# <cch/...> spelling) to dependents and keeps the repository-private src root
# private. There is no global public header root: deleting a package's
# interface root removes its headers from every dependent's include path.
function(cch_owner_include_roots target_name interface_root)
    target_include_directories(${target_name}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/${interface_root}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endfunction()
