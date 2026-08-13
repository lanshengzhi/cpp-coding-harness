# Central Parity Architecture constructors and Gate orchestration.
#
# This module declares the production-target constructor used by fixtures (and,
# in later slices, the real project) and drives the fail-closed Parity
# Architecture Gate. Targets are declared with an explicit role, Owner, source
# set, and direct dependencies; configure records those declarations as the
# resolved ownership index, and the Gate compares that index to the strict
# versioned manifest (see cmake/parity/manifest.json). CMake source formatting
# is not the architecture seam: the emitted index and the manifest are.
#
# See ADR 0039 and CODING_STANDARDS.md section 12.

include_guard(GLOBAL)

set(CCH_PARITY_INDEX_PRODUCER "cch-parity-constructor")
set(CCH_PARITY_INDEX_SCHEMA_VERSION "1")

# Closed role vocabulary. The manifest (cmake/parity/manifest.json) is the
# authority; this local copy only gives fast configure-time feedback before the
# Gate runs.
set(CCH_PARITY_ROLES owner implementation support composition external)

# Project warning defaults applied to every compiled target (CODING_STANDARDS
# section 12.4). Kept local to the constructor so fixtures do not depend on the
# main project's CCH_WARNING_OPTIONS variable.
set(CCH_PARITY_WARNING_OPTIONS -Wall -Wextra -Wpedantic)

# The strict manifest and its validator live next to this module:
#   <module_dir>/parity/manifest.json
#   <module_dir>/parity/parity_gate.py
set(CCH_PARITY_DIR "${CMAKE_CURRENT_LIST_DIR}/parity")
set(CCH_PARITY_GATE_SCRIPT "${CCH_PARITY_DIR}/parity_gate.py")

function(_cch_parity_json_escape input output_variable)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${output_variable} "${escaped}" PARENT_SCOPE)
endfunction()

# cch_parity_declare_target(
#     TARGET <name>
#     ROLE <owner|implementation|support|composition>
#     OWNER <package-name>            # required for every role except `external`
#     SOURCES <path> [<path> ...]
#     DEPENDS <target>|<name@family> [...]
# )
#
# Declares one production target, records its role/Owner/sources/dependencies
# for the ownership index, and creates the authoritative compiled static
# library. A dependency token of the form `name@family` names a classified
# external imported target; a bare token names a project target.
function(cch_parity_declare_target)
    set(options "")
    set(one_value_keywords TARGET ROLE OWNER)
    set(multi_value_keywords SOURCES DEPENDS)
    cmake_parse_arguments(arg "${options}" "${one_value_keywords}" "${multi_value_keywords}" ${ARGN})

    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "cch_parity_declare_target: unexpected positional arguments: "
            "${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT DEFINED arg_TARGET OR "${arg_TARGET}" STREQUAL "")
        message(FATAL_ERROR "cch_parity_declare_target: TARGET is required")
    endif()
    if(NOT DEFINED arg_ROLE OR "${arg_ROLE}" STREQUAL "")
        message(FATAL_ERROR "cch_parity_declare_target: ROLE is required for '${arg_TARGET}'")
    endif()
    if(NOT arg_ROLE IN_LIST CCH_PARITY_ROLES)
        message(FATAL_ERROR
            "cch_parity_declare_target: '${arg_TARGET}' has unknown role '${arg_ROLE}'; "
            "supported roles: ${CCH_PARITY_ROLES}")
    endif()
    if(arg_ROLE STREQUAL "external")
        message(FATAL_ERROR
            "cch_parity_declare_target: '${arg_TARGET}' declares role 'external'; classified "
            "external imported targets are not declared by this constructor")
    endif()
    if(NOT DEFINED arg_OWNER OR "${arg_OWNER}" STREQUAL "")
        message(FATAL_ERROR
            "cch_parity_declare_target: OWNER is required for '${arg_TARGET}' (role '${arg_ROLE}')")
    endif()

    get_property(declared GLOBAL PROPERTY CCH_PARITY_DECLARED_TARGETS)
    if(arg_TARGET IN_LIST declared)
        message(FATAL_ERROR
            "cch_parity_declare_target: target '${arg_TARGET}' is declared more than once")
    endif()

    set(json "{\"name\":\"${arg_TARGET}\",\"role\":\"${arg_ROLE}\",\"owner\":\"${arg_OWNER}\"")
    string(APPEND json ",\"sources\":[")
    set(first TRUE)
    foreach(source IN LISTS arg_SOURCES)
        _cch_parity_json_escape("${source}" escaped_source)
        if(NOT first)
            string(APPEND json ",")
        endif()
        string(APPEND json "\"${escaped_source}\"")
        set(first FALSE)
    endforeach()
    string(APPEND json "]")

    # A dependency token is either a bare project-target name or `name@family`
    # naming a classified external imported target. The bare/external name is
    # the real link target; `family` is only classification metadata for the
    # Gate. Both are recorded in the index and linked into the configured graph.
    string(APPEND json ",\"dependencies\":[")
    set(first TRUE)
    set(link_dependencies "")
    foreach(dependency IN LISTS arg_DEPENDS)
        string(FIND "${dependency}" "@" at_position)
        if(at_position EQUAL -1)
            set(dependency_name "${dependency}")
            set(dependency_family "")
        else()
            string(SUBSTRING "${dependency}" 0 ${at_position} dependency_name)
            math(EXPR family_start "${at_position} + 1")
            string(SUBSTRING "${dependency}" ${family_start} -1 dependency_family)
        endif()
        _cch_parity_json_escape("${dependency_name}" escaped_name)
        _cch_parity_json_escape("${dependency_family}" escaped_family)
        if(NOT first)
            string(APPEND json ",")
        endif()
        if(dependency_family STREQUAL "")
            string(APPEND json "{\"name\":\"${escaped_name}\",\"family\":null}")
        else()
            string(APPEND json "{\"name\":\"${escaped_name}\",\"family\":\"${escaped_family}\"}")
        endif()
        set(first FALSE)
        list(APPEND link_dependencies "${dependency_name}")
    endforeach()
    string(APPEND json "]}")
    set_property(GLOBAL APPEND PROPERTY CCH_PARITY_DECLARATIONS "${json}")
    set_property(GLOBAL APPEND PROPERTY CCH_PARITY_DECLARED_TARGETS "${arg_TARGET}")

    # The declaration is real configured evidence: the authoritative compiled
    # static library carries the project warning defaults and the declared
    # unconditional direct dependencies become actual link edges, so the Gate
    # validates the configured graph rather than source formatting.
    add_library(${arg_TARGET} STATIC ${arg_SOURCES})
    target_compile_options(${arg_TARGET} PRIVATE ${CCH_PARITY_WARNING_OPTIONS})
    if(link_dependencies)
        target_link_libraries(${arg_TARGET} PRIVATE ${link_dependencies})
    endif()
endfunction()

# Emits the resolved ownership index as JSON at `output_path`. The index is
# configured evidence produced by the constructor; it is never a second
# hand-maintained inventory.
function(cch_parity_emit_index output_path)
    if(NOT DEFINED CCH_PARITY_MANIFEST_DIGEST OR "${CCH_PARITY_MANIFEST_DIGEST}" STREQUAL "")
        message(FATAL_ERROR
            "cch_parity_emit_index: call cch_parity_set_manifest() first so the index can record "
            "the manifest digest")
    endif()

    get_property(declarations GLOBAL PROPERTY CCH_PARITY_DECLARATIONS)
    set(json "{\"producer\":\"${CCH_PARITY_INDEX_PRODUCER}\"")
    string(APPEND json ",\"schema_version\":${CCH_PARITY_INDEX_SCHEMA_VERSION}")
    string(APPEND json ",\"manifest_digest\":\"${CCH_PARITY_MANIFEST_DIGEST}\"")
    string(APPEND json ",\"targets\":[")
    set(first TRUE)
    foreach(declaration IN LISTS declarations)
        if(NOT first)
            string(APPEND json ",")
        endif()
        string(APPEND json "${declaration}")
        set(first FALSE)
    endforeach()
    string(APPEND json "]}")
    file(WRITE "${output_path}" "${json}")
endfunction()

# Records the manifest to validate against and captures its content digest for
# the ownership index's freshness identity.
function(cch_parity_set_manifest manifest_path)
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR "Parity Architecture Manifest not found: '${manifest_path}'")
    endif()
    file(SHA256 "${manifest_path}" manifest_digest)
    set(CCH_PARITY_MANIFEST_DIGEST "${manifest_digest}" CACHE INTERNAL
        "SHA-256 digest of the strict Parity Architecture Manifest")
endfunction()

# Runs the fail-closed Gate. Emits the fresh ownership index next to the build
# tree, then invokes the Python validator against the manifest and index; a
# Gate failure fails configuration with the validator's deterministic report.
function(cch_parity_run_gate manifest_path)
    if(NOT EXISTS "${CCH_PARITY_GATE_SCRIPT}")
        message(FATAL_ERROR "Parity Architecture Gate validator not found: '${CCH_PARITY_GATE_SCRIPT}'")
    endif()

    set(index_path "${CMAKE_BINARY_DIR}/parity-ownership-index.json")
    cch_parity_emit_index("${index_path}")

    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
            --manifest "${manifest_path}"
            --index "${index_path}"
            --format human
        RESULT_VARIABLE gate_result
        OUTPUT_VARIABLE gate_output
        ERROR_VARIABLE gate_error
    )
    if(NOT gate_result EQUAL 0)
        message(FATAL_ERROR
            "Parity Architecture Gate rejected the configured graph:\n${gate_output}${gate_error}")
    endif()
    message(STATUS "${gate_output}")
endfunction()
