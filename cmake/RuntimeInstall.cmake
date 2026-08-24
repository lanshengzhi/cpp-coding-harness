# Runtime-only install surface (ADR 0039; issue #472).
#
# `cmake --install` installs only the Pike Runtime and the required
# third-party license/notice texts — no Owner Interface headers, static
# libraries, CMake package metadata, exported targets, components, or other
# development surface. The install path is gated: a generated install-gate
# script runs before any file rule, re-running the build-phase Parity
# Architecture Gate, the Gate-evidence freshness check, and the Runtime
# dependency-closure audit; any rejection aborts the install, so the install
# path cannot bypass architecture validation or ship artifacts the current
# tree has not validated.
#
# See ADR 0039 ("Runtime-only release") and CODING_STANDARDS.md section 12.

include_guard(GLOBAL)

set(CCH_RUNTIME_INSTALL_DIR "${CMAKE_CURRENT_LIST_DIR}/install")

# Resolves the third-party license texts for the Runtime's static link closure
# as `name=path` pairs: one copyright file per linked vcpkg dependency family
# from the pinned manifest install tree, plus the vendored stb license. The
# threads family is the system C library and Catch2 is test-only, so neither
# ships a notice. A missing pinned copyright fails configure: the install
# surface can never silently drop a required notice.
function(cch_runtime_collect_licenses out_var)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR "${VCPKG_INSTALLED_DIR}" STREQUAL "" OR
       NOT DEFINED VCPKG_TARGET_TRIPLET OR "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
        message(FATAL_ERROR
            "cch_runtime_collect_licenses: the pinned vcpkg toolchain is required")
    endif()
    set(share_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    # All Boost ports carry the identical Boost Software License text, so the
    # headers port's copyright is the representative Boost notice.
    set(port_by_family
        "boost=boost-headers"
        "glaze=glaze"
        "libwebp=libwebp"
        "md4c=md4c"
        "openssl=openssl"
        "utf8proc=utf8proc"
    )
    set(licenses "")
    foreach(pair IN LISTS port_by_family)
        string(REGEX MATCH "^([^=]+)=(.+)$" matched "${pair}")
        if(NOT matched)
            message(FATAL_ERROR "cch_runtime_collect_licenses: malformed mapping '${pair}'")
        endif()
        set(path "${share_root}/${CMAKE_MATCH_2}/copyright")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR
                "Pinned vcpkg dependency '${CMAKE_MATCH_2}' provides no copyright file at "
                "'${path}'; the Runtime install cannot drop its required notice")
        endif()
        list(APPEND licenses "${CMAKE_MATCH_1}=${path}")
    endforeach()
    set(stb_license "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/LICENSE")
    if(NOT EXISTS "${stb_license}")
        message(FATAL_ERROR "Vendored stb license not found: '${stb_license}'")
    endif()
    list(APPEND licenses "stb=${stb_license}")
    set(${out_var} "${licenses}" PARENT_SCOPE)
endfunction()

# cch_runtime_install_rules(
#     TARGET <composition-executable-target>
#     MANIFEST <path>
#     PROJECT_ROOT <path>
#     LICENSE_FILES <name=path> [...]
#     [EXTERNAL_INCLUDE_ROOTS <path> [...]]
# )
#
# Declares the Runtime-only install surface, in order: the fail-closed install
# gate (an install SCRIPT that runs before any file lands in the prefix), the
# Runtime executable under bin/, and the license texts as
# share/<target>/licenses/<name>.txt. MANIFEST, PROJECT_ROOT, and
# EXTERNAL_INCLUDE_ROOTS are the same Parity Architecture Gate inputs the
# configure/build phases use; the generated gate revalidates them at install
# time.
function(cch_runtime_install_rules)
    set(options "")
    set(one_value_keywords TARGET MANIFEST PROJECT_ROOT)
    set(multi_value_keywords LICENSE_FILES EXTERNAL_INCLUDE_ROOTS)
    cmake_parse_arguments(arg "${options}" "${one_value_keywords}" "${multi_value_keywords}" ${ARGN})

    foreach(required TARGET MANIFEST PROJECT_ROOT LICENSE_FILES)
        if(NOT DEFINED arg_${required} OR "${arg_${required}}" STREQUAL "")
            message(FATAL_ERROR "cch_runtime_install_rules: ${required} is required")
        endif()
    endforeach()
    if(NOT TARGET ${arg_TARGET})
        message(FATAL_ERROR "cch_runtime_install_rules: unknown target '${arg_TARGET}'")
    endif()

    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

    set(build_dir "${CMAKE_BINARY_DIR}")

    # Dependency-closure audit forbidden roots: the resolved closure may never
    # enter the source tree, the build tree, or the pinned vcpkg tree. The
    # fragment is configure-controlled absolute paths; reject quotes outright
    # so the generated script stays injection-free.
    set(forbid_roots "${arg_PROJECT_ROOT}" "${build_dir}")
    if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "" AND
       DEFINED VCPKG_TARGET_TRIPLET AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
        list(APPEND forbid_roots "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    endif()
    set(CCH_IG_FORBID_ROOT_ARGS "")
    foreach(root IN LISTS forbid_roots)
        if(root MATCHES "\"")
            message(FATAL_ERROR "cch_runtime_install_rules: path may not contain '\"': '${root}'")
        endif()
        string(APPEND CCH_IG_FORBID_ROOT_ARGS
            "\n        \"--forbid-root\" \"${root}\"")
    endforeach()

    # Bake the Gate inputs into the install gate; the $<TARGET_FILE:...>
    # generator expression survives string(CONFIGURE) and is evaluated by
    # file(GENERATE) once the target's output name is known.
    set(CCH_IG_TARGET "${arg_TARGET}")
    set(CCH_IG_MANIFEST "${arg_MANIFEST}")
    set(CCH_IG_PROJECT_ROOT "${arg_PROJECT_ROOT}")
    set(CCH_IG_BUILD_DIR "${build_dir}")
    set(CCH_IG_EXTERNAL_ROOTS "${arg_EXTERNAL_INCLUDE_ROOTS}")
    file(READ "${CCH_RUNTIME_INSTALL_DIR}/InstallGate.cmake.in" gate_template)
    string(CONFIGURE "${gate_template}" gate_configured @ONLY)
    file(GENERATE
        OUTPUT "${build_dir}/cch-install-gate-$<CONFIG>.cmake"
        CONTENT "${gate_configured}")

    # The gate runs before any file rule: a rejection aborts the install with
    # nothing staged.
    install(SCRIPT "${build_dir}/cch-install-gate-$<CONFIG>.cmake")

    # The Runtime executable is the whole installed product surface.
    install(TARGETS ${arg_TARGET} RUNTIME DESTINATION bin)

    foreach(pair IN LISTS arg_LICENSE_FILES)
        string(REGEX MATCH "^([^=]+)=(.+)$" matched "${pair}")
        if(NOT matched)
            message(FATAL_ERROR
                "cch_runtime_install_rules: malformed LICENSE_FILES entry '${pair}'")
        endif()
        set(license_path "${CMAKE_MATCH_2}")
        if(NOT EXISTS "${license_path}")
            message(FATAL_ERROR
                "cch_runtime_install_rules: license file not found: '${license_path}'")
        endif()
        install(FILES "${license_path}"
            DESTINATION "share/${arg_TARGET}/licenses"
            RENAME "${CMAKE_MATCH_1}.txt")
    endforeach()
endfunction()
