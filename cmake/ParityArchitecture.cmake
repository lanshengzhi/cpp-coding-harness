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

# Closed target-kind vocabulary for the constructor: a compiled static library
# (default), a header-only interface library, or the final composition
# executable. The kind only changes how the real target is created; the
# ownership index records role/Owner/sources/dependencies identically.
set(CCH_PARITY_TARGET_KINDS static interface executable)

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

# Splits a dependency token into its bare link-target name and optional
# external family. A bare token `name` yields an empty family; `name@family`
# yields the family after the `@`.
function(_cch_parity_split_dependency token name_var family_var)
    string(FIND "${token}" "@" at_position)
    if(at_position EQUAL -1)
        set(name "${token}")
        set(family "")
    else()
        string(SUBSTRING "${token}" 0 ${at_position} name)
        math(EXPR family_start "${at_position} + 1")
        string(SUBSTRING "${token}" ${family_start} -1 family)
    endif()
    set(${name_var} "${name}" PARENT_SCOPE)
    set(${family_var} "${family}" PARENT_SCOPE)
endfunction()

# cch_parity_declare_target(
#     TARGET <name>
#     ROLE <owner|implementation|support|composition>
#     OWNER <package-name>            # required for every role except `external`
#     KIND <static|interface|executable>  # default `static`
#     SOURCES <path> [<path> ...]
#     DEPENDS <target>|<name@family> [...]
#     INTERFACE_DEPENDS <target>|<name@family> [...]  # PUBLIC (interface-visible) subset of DEPENDS
#     FORCED_INCLUDES <path> [<path> ...]   # declared, scanned forced includes
#     PCH_INPUT <path>                       # declared, scanned PCH input source
# )
#
# Declares one production target, records its role/Owner/sources/dependencies
# (including interface visibility) and declared compile context for the
# ownership index, and creates the real compiled target. A dependency token of
# the form `name@family` names a classified external imported target; a bare
# token names a project target. INTERFACE_DEPENDS names the DEPENDS subset
# linked PUBLIC (recorded as `visibility: "public"`); every other dependency
# is linked PRIVATE. Forced includes and the PCH input are declared and
# scanned by the direct-include lexer; opaque `.gch`/`.pch` artifacts are
# never declared here.
function(cch_parity_declare_target)
    set(options "")
    set(one_value_keywords TARGET ROLE OWNER KIND PCH_INPUT)
    set(multi_value_keywords SOURCES DEPENDS INTERFACE_DEPENDS FORCED_INCLUDES)
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

    # Target kind: a compiled static library (default), a header-only interface
    # library, or the final composition executable. The kind only changes how
    # the real target is created; role, Owner, sources, and dependencies are
    # recorded identically in the ownership index.
    if(NOT DEFINED arg_KIND OR "${arg_KIND}" STREQUAL "")
        set(kind "static")
    else()
        set(kind "${arg_KIND}")
    endif()
    if(NOT kind IN_LIST CCH_PARITY_TARGET_KINDS)
        message(FATAL_ERROR
            "cch_parity_declare_target: '${arg_TARGET}' has unknown KIND '${kind}'; "
            "supported kinds: ${CCH_PARITY_TARGET_KINDS}")
    endif()
    if(kind STREQUAL "interface" AND arg_SOURCES)
        message(FATAL_ERROR
            "cch_parity_declare_target: interface target '${arg_TARGET}' must not declare SOURCES")
    endif()

    # Canonicalize declared paths to absolute form so the ownership index, the
    # direct-include scan, and the generated compile commands share one
    # spelling (the build-phase Gate compares them by absolute path).
    set(abs_sources "")
    foreach(source IN LISTS arg_SOURCES)
        if(IS_ABSOLUTE "${source}")
            list(APPEND abs_sources "${source}")
        else()
            list(APPEND abs_sources "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
    endforeach()
    set(abs_forced_includes "")
    foreach(forced IN LISTS arg_FORCED_INCLUDES)
        if(IS_ABSOLUTE "${forced}")
            list(APPEND abs_forced_includes "${forced}")
        else()
            list(APPEND abs_forced_includes "${CMAKE_CURRENT_SOURCE_DIR}/${forced}")
        endif()
    endforeach()
    if(DEFINED arg_PCH_INPUT AND NOT "${arg_PCH_INPUT}" STREQUAL "")
        if(IS_ABSOLUTE "${arg_PCH_INPUT}")
            set(abs_pch_input "${arg_PCH_INPUT}")
        else()
            set(abs_pch_input "${CMAKE_CURRENT_SOURCE_DIR}/${arg_PCH_INPUT}")
        endif()
    else()
        set(abs_pch_input "")
    endif()

    get_property(declared GLOBAL PROPERTY CCH_PARITY_DECLARED_TARGETS)
    if(arg_TARGET IN_LIST declared)
        message(FATAL_ERROR
            "cch_parity_declare_target: target '${arg_TARGET}' is declared more than once")
    endif()

    # A dependency token is either a bare project-target name or `name@family`
    # naming a classified external imported target. The bare/external name is
    # the real link target; `family` is only classification metadata for the
    # Gate. INTERFACE_DEPENDS names the subset of DEPENDS that is interface-
    # visible (linked PUBLIC); every other dependency is linked PRIVATE.
    # Visibility is recorded in the index so the Gate can audit interface
    # leakage without re-reading CMake source text.
    set(depends_names "")
    foreach(dependency IN LISTS arg_DEPENDS)
        _cch_parity_split_dependency("${dependency}" dependency_name dependency_family)
        list(APPEND depends_names "${dependency_name}")
    endforeach()

    set(interface_names "")
    foreach(interface_dep IN LISTS arg_INTERFACE_DEPENDS)
        _cch_parity_split_dependency("${interface_dep}" interface_name interface_family)
        if(NOT interface_name IN_LIST depends_names)
            message(FATAL_ERROR
                "cch_parity_declare_target: '${arg_TARGET}' marks '${interface_name}' as "
                "interface-visible but it is not listed in DEPENDS")
        endif()
        list(APPEND interface_names "${interface_name}")
    endforeach()

    set(json "{\"name\":\"${arg_TARGET}\",\"role\":\"${arg_ROLE}\",\"owner\":\"${arg_OWNER}\"")
    string(APPEND json ",\"sources\":[")
    set(first TRUE)
    foreach(source IN LISTS abs_sources)
        _cch_parity_json_escape("${source}" escaped_source)
        if(NOT first)
            string(APPEND json ",")
        endif()
        string(APPEND json "\"${escaped_source}\"")
        set(first FALSE)
    endforeach()
    string(APPEND json "]")
    string(APPEND json ",\"dependencies\":[")
    set(first TRUE)
    set(link_dependencies "")
    set(interface_link_dependencies "")
    set(private_link_dependencies "")
    foreach(dependency IN LISTS arg_DEPENDS)
        _cch_parity_split_dependency("${dependency}" dependency_name dependency_family)
        _cch_parity_json_escape("${dependency_name}" escaped_name)
        if(dependency_family STREQUAL "")
            set(family_json "null")
        else()
            _cch_parity_json_escape("${dependency_family}" escaped_family)
            set(family_json "\"${escaped_family}\"")
        endif()
        if(kind STREQUAL "interface" OR dependency_name IN_LIST interface_names)
            set(visibility "\"public\"")
        else()
            set(visibility "\"private\"")
        endif()
        if(NOT first)
            string(APPEND json ",")
        endif()
        string(APPEND json "{\"name\":\"${escaped_name}\",\"family\":${family_json},\"visibility\":${visibility}}")
        set(first FALSE)
        list(APPEND link_dependencies "${dependency_name}")
        if(kind STREQUAL "interface" OR dependency_name IN_LIST interface_names)
            list(APPEND interface_link_dependencies "${dependency_name}")
        else()
            list(APPEND private_link_dependencies "${dependency_name}")
        endif()
    endforeach()
    # Declared compile context: forced includes and the PCH input source are
    # recorded in the index so the Gate can validate that every forced include
    # and PCH input is declared and scanned, and that no opaque `.gch`/`.pch`
    # artifact sneaks into a compile command.
    string(APPEND json "]")
    string(APPEND json ",\"forced_includes\":[")
    set(first TRUE)
    foreach(forced IN LISTS abs_forced_includes)
        _cch_parity_json_escape("${forced}" escaped_forced)
        if(NOT first)
            string(APPEND json ",")
        endif()
        string(APPEND json "\"${escaped_forced}\"")
        set(first FALSE)
    endforeach()
    string(APPEND json "]")
    if(NOT "${abs_pch_input}" STREQUAL "")
        _cch_parity_json_escape("${abs_pch_input}" escaped_pch)
        string(APPEND json ",\"pch_input\":\"${escaped_pch}\"")
    else()
        string(APPEND json ",\"pch_input\":null")
    endif()

    string(APPEND json "}")
    set_property(GLOBAL APPEND PROPERTY CCH_PARITY_DECLARATIONS "${json}")
    set_property(GLOBAL APPEND PROPERTY CCH_PARITY_DECLARED_TARGETS "${arg_TARGET}")
    set_property(GLOBAL APPEND PROPERTY CCH_PARITY_SOURCES ${abs_sources})
    set_property(GLOBAL APPEND PROPERTY CCH_PARITY_FORCED_INCLUDES ${abs_forced_includes})
    if(NOT "${abs_pch_input}" STREQUAL "")
        set_property(GLOBAL APPEND PROPERTY CCH_PARITY_PCH_INPUTS "${abs_pch_input}")
    endif()

    # The declaration is real configured evidence: the target carries the
    # project warning defaults, interface-visible dependencies become PUBLIC
    # link edges, private dependencies stay PRIVATE, and declared forced
    # includes/PCH input become real compile context the Gate validates.
    if(kind STREQUAL "interface")
        add_library(${arg_TARGET} INTERFACE)
    elseif(kind STREQUAL "executable")
        add_executable(${arg_TARGET} ${abs_sources})
        target_compile_options(${arg_TARGET} PRIVATE ${CCH_PARITY_WARNING_OPTIONS})
    else()
        add_library(${arg_TARGET} STATIC ${abs_sources})
        target_compile_options(${arg_TARGET} PRIVATE ${CCH_PARITY_WARNING_OPTIONS})
    endif()
    if(NOT kind STREQUAL "interface")
        foreach(forced IN LISTS abs_forced_includes)
            target_compile_options(${arg_TARGET} PRIVATE -include "${forced}")
        endforeach()
        if(NOT "${abs_pch_input}" STREQUAL "")
            target_compile_options(${arg_TARGET} PRIVATE -include "${abs_pch_input}")
        endif()
    endif()
    if(link_dependencies)
        if(kind STREQUAL "interface")
            target_link_libraries(${arg_TARGET} INTERFACE ${link_dependencies})
        elseif(interface_link_dependencies AND private_link_dependencies)
            target_link_libraries(${arg_TARGET}
                PUBLIC ${interface_link_dependencies}
                PRIVATE ${private_link_dependencies})
        elseif(interface_link_dependencies)
            target_link_libraries(${arg_TARGET} PUBLIC ${interface_link_dependencies})
        else()
            target_link_libraries(${arg_TARGET} PRIVATE ${private_link_dependencies})
        endif()
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

# Runs the fail-closed Gate at configure time. Emits the fresh ownership index
# and the direct-include lexer output next to the build tree, then invokes the
# Python validator against the manifest, index, and direct-include evidence; a
# Gate failure fails configuration with the validator's deterministic report.
# This Gate runs on every supported configure, including configurations with
# tests disabled. PROJECT_ROOT is the directory against which manifest
# interface roots resolve. EXTERNAL_INCLUDE_ROOTS names external dependency
# include roots (for example the vcpkg installed include directory) that are
# never project paths.
#
# Compile commands and depfiles are produced after generation/build, so the
# build-phase Gate is driven by cch_parity_add_build_gate (see
# tests/architecture/ParityGateTest.cmake and cmake/parity/run-build-gate.cmake).
function(cch_parity_run_gate manifest_path)
    set(options "")
    set(one_value_keywords PROJECT_ROOT STRICT_NO_EXCEPTIONS)
    set(multi_value_keywords EXTERNAL_INCLUDE_ROOTS)
    cmake_parse_arguments(gate "${options}" "${one_value_keywords}" "${multi_value_keywords}" ${ARGN})

    if(NOT EXISTS "${CCH_PARITY_GATE_SCRIPT}")
        message(FATAL_ERROR "Parity Architecture Gate validator not found: '${CCH_PARITY_GATE_SCRIPT}'")
    endif()

    set(index_path "${CMAKE_BINARY_DIR}/parity-ownership-index.json")
    cch_parity_emit_index("${index_path}")

    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

    # Scan every declared source, forced include, and PCH input in all source
    # text branches, producing the direct-include evidence the Gate resolves.
    get_property(scan_sources GLOBAL PROPERTY CCH_PARITY_SOURCES)
    get_property(scan_forced GLOBAL PROPERTY CCH_PARITY_FORCED_INCLUDES)
    get_property(scan_pch GLOBAL PROPERTY CCH_PARITY_PCH_INPUTS)
    set(direct_includes_path "${CMAKE_BINARY_DIR}/parity-direct-includes.json")
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
            --out "${direct_includes_path}"
            --sources ${scan_sources} ${scan_forced} ${scan_pch}
        RESULT_VARIABLE scan_result
        OUTPUT_VARIABLE scan_output
        ERROR_VARIABLE scan_error
    )
    if(NOT scan_result EQUAL 0)
        message(FATAL_ERROR
            "Parity Architecture Gate could not scan direct includes:\n${scan_output}${scan_error}")
    endif()

    set(gate_command
        "${Python3_EXECUTABLE}" "${CCH_PARITY_GATE_SCRIPT}"
        --manifest "${manifest_path}"
        --index "${index_path}"
        --direct-includes "${direct_includes_path}"
        --format human
    )
    if(DEFINED gate_PROJECT_ROOT AND NOT "${gate_PROJECT_ROOT}" STREQUAL "")
        list(APPEND gate_command --project-root "${gate_PROJECT_ROOT}")
    endif()
    if(DEFINED gate_EXTERNAL_INCLUDE_ROOTS)
        foreach(external_root IN LISTS gate_EXTERNAL_INCLUDE_ROOTS)
            list(APPEND gate_command --external-include-root "${external_root}")
        endforeach()
    endif()
    if(gate_STRICT_NO_EXCEPTIONS)
        list(APPEND gate_command --strict-no-exceptions)
    endif()
    execute_process(
        COMMAND ${gate_command}
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

# Wires the build-phase Parity Architecture Gate into the build graph (ADR
# 0039; issue #470). Creates the `cch_parity_gate_build` custom target, a
# member of `all`, that depends on every declared production target so its
# command runs after all production compilation has produced fresh depfiles;
# the command records the active dependency (depfile) evidence and validates
# manifest, index, direct-include, compile-command, and depfile evidence at
# the build phase, failing the build on any rejection.
#
# Test executables must additionally depend on ${CCH_PARITY_BUILD_GATE_TARGET}
# so CTest entry points require fresh successful Gate evidence. EXTERNAL_
# INCLUDE_ROOTS names external dependency include roots that are never project
# paths. The target name is published as the CACHE INTERNAL variable
# CCH_PARITY_BUILD_GATE_TARGET.
function(cch_parity_add_build_gate manifest_path)
    set(options "")
    set(one_value_keywords PROJECT_ROOT BUILD_DIR STRICT_NO_EXCEPTIONS)
    set(multi_value_keywords EXTERNAL_INCLUDE_ROOTS)
    cmake_parse_arguments(gate "${options}" "${one_value_keywords}" "${multi_value_keywords}" ${ARGN})

    if(NOT DEFINED gate_BUILD_DIR OR "${gate_BUILD_DIR}" STREQUAL "")
        set(gate_BUILD_DIR "${CMAKE_BINARY_DIR}")
    endif()
    if(NOT DEFINED gate_PROJECT_ROOT OR "${gate_PROJECT_ROOT}" STREQUAL "")
        set(gate_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    get_property(production_targets GLOBAL PROPERTY CCH_PARITY_DECLARED_TARGETS)
    if(NOT production_targets)
        message(FATAL_ERROR
            "cch_parity_add_build_gate: no production targets are declared; call it after "
            "every cch_parity_declare_target")
    endif()

    set(build_gate_target "cch_parity_gate_build")
    set(CCH_PARITY_BUILD_GATE_TARGET "${build_gate_target}" CACHE INTERNAL
        "Target that runs the build-phase Parity Architecture Gate")
    set(CCH_PARITY_BUILD_GATE_MANIFEST "${manifest_path}" CACHE INTERNAL
        "Strict Parity Architecture Manifest path used by the build-phase Gate")
    set(CCH_PARITY_BUILD_GATE_PROJECT_ROOT "${gate_PROJECT_ROOT}" CACHE INTERNAL
        "Project root used by the build-phase Gate")
    set(CCH_PARITY_BUILD_GATE_EXTERNAL_ROOTS "${gate_EXTERNAL_INCLUDE_ROOTS}" CACHE INTERNAL
        "External dependency include roots used by the build-phase Gate")
    set(CCH_PARITY_BUILD_GATE_STRICT_NO_EXCEPTIONS "${gate_STRICT_NO_EXCEPTIONS}" CACHE INTERNAL
        "Whether the build-phase Gate enforces the strict no-exception policy")
    add_custom_target(${build_gate_target} ALL
        COMMAND
            "${CMAKE_COMMAND}"
            -DCCH_PARITY_GATE_SCRIPT=${CCH_PARITY_GATE_SCRIPT}
            -DCCH_PARITY_MANIFEST=${manifest_path}
            -DCCH_PARITY_INDEX=${gate_BUILD_DIR}/parity-ownership-index.json
            -DCCH_PARITY_DIRECT_INCLUDES=${gate_BUILD_DIR}/parity-direct-includes.json
            -DCCH_PARITY_COMPILE_COMMANDS=${gate_BUILD_DIR}/compile_commands.json
            -DCCH_PARITY_PROJECT_ROOT=${gate_PROJECT_ROOT}
            -DCCH_PARITY_DEPFILES=${gate_BUILD_DIR}/parity-build-gate-depfiles.json
            -DCCH_PARITY_REPORT=${gate_BUILD_DIR}/parity-build-gate.json
            $<$<BOOL:${gate_EXTERNAL_INCLUDE_ROOTS}>:-DCCH_PARITY_EXTERNAL_INCLUDE_ROOTS=${gate_EXTERNAL_INCLUDE_ROOTS}>
            -DCCH_PARITY_STRICT_NO_EXCEPTIONS=${gate_STRICT_NO_EXCEPTIONS}
            -P ${CCH_PARITY_DIR}/run-build-gate.cmake
        DEPENDS ${production_targets}
        COMMENT "Parity Architecture Gate (build phase)"
        VERBATIM
    )

    set(CCH_PARITY_BUILD_GATE_SCRIPT ${CCH_PARITY_DIR}/run-build-gate.cmake CACHE INTERNAL
        "CMake script that runs the build-phase Parity Architecture Gate")
    set(CCH_PARITY_BUILD_GATE_REPORT ${gate_BUILD_DIR}/parity-build-gate.json CACHE INTERNAL
        "Path to the machine-readable build-phase Gate report")
endfunction()

# Attaches the build-phase Parity Architecture Gate as a POST_BUILD step of a
# production target. Used on the final composition executable so that a direct
# `--target cpp_harness` production-target build still requires fresh
# successful Gate evidence: by the time the composition target's POST_BUILD
# runs, every production source (including its own entry point) has compiled
# and produced a fresh depfile, so the same evidence set is available that the
# `cch_parity_gate_build` target validates on an `all` build.
function(cch_parity_attach_build_gate_post_build target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "cch_parity_attach_build_gate_post_build: unknown target '${target_name}'")
    endif()
    if(NOT DEFINED CCH_PARITY_BUILD_GATE_SCRIPT OR NOT DEFINED CCH_PARITY_BUILD_GATE_REPORT OR
       NOT DEFINED CCH_PARITY_BUILD_GATE_MANIFEST)
        message(FATAL_ERROR
            "cch_parity_attach_build_gate_post_build: call cch_parity_add_build_gate() first")
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}"
            -DCCH_PARITY_GATE_SCRIPT=${CCH_PARITY_GATE_SCRIPT}
            -DCCH_PARITY_MANIFEST=${CCH_PARITY_BUILD_GATE_MANIFEST}
            -DCCH_PARITY_INDEX=${CMAKE_BINARY_DIR}/parity-ownership-index.json
            -DCCH_PARITY_DIRECT_INCLUDES=${CMAKE_BINARY_DIR}/parity-direct-includes.json
            -DCCH_PARITY_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
            -DCCH_PARITY_PROJECT_ROOT=${CCH_PARITY_BUILD_GATE_PROJECT_ROOT}
            -DCCH_PARITY_DEPFILES=${CMAKE_BINARY_DIR}/parity-build-gate-depfiles.json
            -DCCH_PARITY_REPORT=${CCH_PARITY_BUILD_GATE_REPORT}
            $<$<BOOL:${CCH_PARITY_BUILD_GATE_EXTERNAL_ROOTS}>:-DCCH_PARITY_EXTERNAL_INCLUDE_ROOTS=${CCH_PARITY_BUILD_GATE_EXTERNAL_ROOTS}>
            -DCCH_PARITY_STRICT_NO_EXCEPTIONS=${CCH_PARITY_BUILD_GATE_STRICT_NO_EXCEPTIONS}
            -P ${CCH_PARITY_BUILD_GATE_SCRIPT}
        VERBATIM
    )
endfunction()

