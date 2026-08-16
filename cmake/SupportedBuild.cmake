include_guard(GLOBAL)

set(CCH_MINIMUM_NINJA_VERSION "1.11")
set(CCH_GCC_MAJOR_VERSION "16")
set(CCH_CLANG_MAJOR_VERSION "22")

function(cch_validate_platform
    system_name
    system_processor
    cross_compiling
    sysroot
    compiler_target
    has_glibc)
    if(cross_compiling OR NOT "${sysroot}" STREQUAL "" OR NOT "${compiler_target}" STREQUAL "")
        message(FATAL_ERROR
            "Cross-compilation is unsupported. Configure natively on Linux x86-64 with glibc; "
            "remove the target toolchain, compiler target, and sysroot settings.")
    endif()

    if(NOT system_name STREQUAL "Linux")
        message(FATAL_ERROR
            "Unsupported operating system '${system_name}'. The supported build host and target "
            "are native Linux x86-64 with glibc.")
    endif()

    string(TOLOWER "${system_processor}" normalized_processor)
    if(NOT normalized_processor STREQUAL "x86_64" AND NOT normalized_processor STREQUAL "amd64")
        message(FATAL_ERROR
            "Unsupported processor '${system_processor}'. The supported architecture is x86-64.")
    endif()

    if(NOT has_glibc)
        message(FATAL_ERROR
            "Unsupported C library. The supported Linux target requires glibc; musl and other C "
            "libraries are not supported.")
    endif()
endfunction()

function(cch_validate_generator generator ninja_version unity_build)
    if(NOT generator STREQUAL "Ninja")
        message(FATAL_ERROR
            "Unsupported CMake generator '${generator}'. Use the Ninja generator through a "
            "checked-in configure preset.")
    endif()

    if(ninja_version VERSION_LESS CCH_MINIMUM_NINJA_VERSION)
        message(FATAL_ERROR
            "Ninja ${ninja_version} is unsupported. Install Ninja 1.11 or newer and reconfigure.")
    endif()

    if(unity_build)
        message(FATAL_ERROR
            "Unity Build is unsupported. Remove CMAKE_UNITY_BUILD and configure a normal Ninja build.")
    endif()
endfunction()

function(cch_validate_compiler_role role compiler_id compiler_version)
    math(EXPR gcc_next_major "${CCH_GCC_MAJOR_VERSION} + 1")
    math(EXPR clang_next_major "${CCH_CLANG_MAJOR_VERSION} + 1")

    if(role STREQUAL "build")
        if(NOT compiler_id STREQUAL "GNU")
            message(FATAL_ERROR
                "The build role requires GCC ${CCH_GCC_MAJOR_VERSION}.x; found "
                "${compiler_id} ${compiler_version}. Use the clang-conformance preset only for "
                "Clang verification.")
        endif()
        if(compiler_version VERSION_LESS CCH_GCC_MAJOR_VERSION OR
           NOT compiler_version VERSION_LESS gcc_next_major)
            message(FATAL_ERROR
                "The build role requires GCC ${CCH_GCC_MAJOR_VERSION}.x; found GCC ${compiler_version}.")
        endif()
    elseif(role STREQUAL "conformance")
        if(NOT compiler_id STREQUAL "Clang")
            message(FATAL_ERROR
                "The conformance role requires Clang ${CCH_CLANG_MAJOR_VERSION}.x; found "
                "${compiler_id} ${compiler_version}.")
        endif()
        if(compiler_version VERSION_LESS CCH_CLANG_MAJOR_VERSION OR
           NOT compiler_version VERSION_LESS clang_next_major)
            message(FATAL_ERROR
                "The conformance role requires Clang ${CCH_CLANG_MAJOR_VERSION}.x; found "
                "Clang ${compiler_version}.")
        endif()
    else()
        message(FATAL_ERROR
            "Unknown CCH_TOOLCHAIN_ROLE '${role}'. Supported roles are 'build' and 'conformance'.")
    endif()
endfunction()

function(cch_validate_configuration_role role build_type)
    if(role STREQUAL "conformance" AND NOT build_type STREQUAL "Debug")
        message(FATAL_ERROR
            "The Clang conformance role requires a Debug configuration; found '${build_type}'. "
            "Clang conformance builds do not produce release artifacts.")
    endif()
endfunction()

# Sanitizer coverage (issue #473): the supported selections are empty (no
# sanitizer), 'address;undefined' (ASan+UBSan), and 'thread' (TSan). TSan
# cannot be combined with any other sanitizer. Unknown or combined values fail
# closed so a typo cannot silently configure an unsanitized "sanitizer" build.
function(cch_validate_sanitizer_selection sanitizers)
    if("${sanitizers}" STREQUAL "")
        return()
    endif()

    set(seen "")
    foreach(sanitizer IN LISTS sanitizers)
        if(NOT sanitizer STREQUAL "address" AND
           NOT sanitizer STREQUAL "undefined" AND
           NOT sanitizer STREQUAL "thread")
            message(FATAL_ERROR
                "Unknown CCH_SANITIZER entry '${sanitizer}'. Supported selections are "
                "'address;undefined' and 'thread'.")
        endif()
        if(sanitizer IN_LIST seen)
            message(FATAL_ERROR
                "Duplicate CCH_SANITIZER entry '${sanitizer}'. List each sanitizer at most once.")
        endif()
        list(APPEND seen "${sanitizer}")
    endforeach()

    if("thread" IN_LIST seen AND NOT "${sanitizers}" STREQUAL "thread")
        message(FATAL_ERROR
            "ThreadSanitizer cannot be combined with other sanitizers; configure CCH_SANITIZER=thread "
            "in its own build tree.")
    endif()
endfunction()

function(cch_validate_vcpkg_shape toolchain_file manifest_mode manifest_install target_triplet)
    if(toolchain_file STREQUAL "")
        message(FATAL_ERROR
            "The pinned vcpkg toolchain is required. Set VCPKG_ROOT and configure through a "
            "checked-in preset; system dependency fallback is unsupported.")
    endif()

    cmake_path(CONVERT "${toolchain_file}" TO_CMAKE_PATH_LIST normalized_toolchain NORMALIZE)
    if(NOT normalized_toolchain MATCHES "/scripts/buildsystems/vcpkg\\.cmake$")
        message(FATAL_ERROR
            "Unsupported dependency toolchain '${toolchain_file}'. Use the pinned vcpkg toolchain "
            "at <VCPKG_ROOT>/scripts/buildsystems/vcpkg.cmake.")
    endif()

    if(NOT manifest_mode)
        message(FATAL_ERROR
            "vcpkg manifest mode is required. Classic mode and system dependency fallback are unsupported.")
    endif()

    if(NOT manifest_install)
        message(FATAL_ERROR
            "vcpkg manifest installation is required. Reusing an unmanaged installed dependency tree "
            "is unsupported; remove VCPKG_MANIFEST_INSTALL=OFF and reconfigure.")
    endif()

    if(NOT target_triplet STREQUAL "x64-linux")
        message(FATAL_ERROR
            "Unsupported vcpkg target triplet '${target_triplet}'. Use native x64-linux dependencies.")
    endif()
endfunction()

function(cch_validate_vcpkg_options
    host_triplet
    overlay_ports
    overlay_triplets
    chainload_toolchain)
    if(NOT host_triplet STREQUAL "x64-linux")
        message(FATAL_ERROR
            "Unsupported vcpkg host triplet '${host_triplet}'. Use native x64-linux tools.")
    endif()
    if(NOT "${overlay_ports}" STREQUAL "" OR
       NOT "${overlay_triplets}" STREQUAL "" OR
       NOT "${chainload_toolchain}" STREQUAL "")
        message(FATAL_ERROR
            "vcpkg overlay ports, overlay triplets, and chainloaded toolchains are unsupported; "
            "use only the pinned manifest dependency graph.")
    endif()
endfunction()

function(cch_validate_vcpkg_dependency_path dependency_name installed_prefix resolved_path)
    if(resolved_path STREQUAL "")
        message(FATAL_ERROR
            "Dependency '${dependency_name}' did not report a resolved path from the pinned vcpkg prefix.")
    endif()

    file(REAL_PATH "${installed_prefix}" normalized_prefix)
    file(REAL_PATH "${resolved_path}" normalized_path)
    cmake_path(IS_PREFIX normalized_prefix "${normalized_path}" NORMALIZE is_vcpkg_path)
    if(NOT is_vcpkg_path)
        message(FATAL_ERROR
            "Dependency '${dependency_name}' resolved outside the pinned vcpkg prefix: "
            "'${resolved_path}'. System-package and find_library/find_path fallback paths are unsupported.")
    endif()
endfunction()

function(cch_vcpkg_root_from_toolchain toolchain_file output_variable)
    file(REAL_PATH "${toolchain_file}" real_toolchain)
    cmake_path(GET real_toolchain PARENT_PATH buildsystems_dir)
    cmake_path(GET buildsystems_dir PARENT_PATH scripts_dir)
    cmake_path(GET scripts_dir PARENT_PATH vcpkg_root)
    set(${output_variable} "${vcpkg_root}" PARENT_SCOPE)
endfunction()

function(cch_validate_supported_build_before_project)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "Unsupported build host '${CMAKE_HOST_SYSTEM_NAME}'. Configure natively on Linux x86-64 with glibc.")
    endif()

    cmake_host_system_information(RESULT host_processor QUERY OS_PLATFORM)
    string(TOLOWER "${host_processor}" normalized_host_processor)
    if(NOT normalized_host_processor STREQUAL "x86_64" AND
       NOT normalized_host_processor STREQUAL "amd64")
        message(FATAL_ERROR
            "Unsupported build host processor '${host_processor}'. Configure natively on Linux x86-64.")
    endif()

    if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
        message(FATAL_ERROR
            "Unsupported CMake generator '${CMAKE_GENERATOR}'. Use the Ninja generator through a "
            "checked-in configure preset.")
    endif()

    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
        cch_validate_vcpkg_shape(
            ""
            "${VCPKG_MANIFEST_MODE}"
            "${VCPKG_MANIFEST_INSTALL}"
            "${VCPKG_TARGET_TRIPLET}")
    endif()

    cch_validate_vcpkg_shape(
        "${CMAKE_TOOLCHAIN_FILE}"
        "${VCPKG_MANIFEST_MODE}"
        "${VCPKG_MANIFEST_INSTALL}"
        "${VCPKG_TARGET_TRIPLET}")
    cch_validate_vcpkg_options(
        "${VCPKG_HOST_TRIPLET}"
        "${VCPKG_OVERLAY_PORTS}"
        "${VCPKG_OVERLAY_TRIPLETS}"
        "${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")
    if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
        message(FATAL_ERROR
            "The configured vcpkg toolchain does not exist: '${CMAKE_TOOLCHAIN_FILE}'. Set VCPKG_ROOT "
            "to a checkout pinned to vcpkg.json's builtin-baseline.")
    endif()

    cch_vcpkg_root_from_toolchain("${CMAKE_TOOLCHAIN_FILE}" vcpkg_root)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" vcpkg_manifest)
    string(JSON expected_baseline ERROR_VARIABLE baseline_error GET "${vcpkg_manifest}" builtin-baseline)
    if(NOT baseline_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Could not read builtin-baseline from vcpkg.json: ${baseline_error}")
    endif()

    execute_process(
        COMMAND git -C "${vcpkg_root}" rev-parse HEAD
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE actual_vcpkg_revision
        ERROR_VARIABLE git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR
            "The vcpkg root '${vcpkg_root}' is not a readable pinned Git checkout: ${git_error}")
    endif()
    if(NOT actual_vcpkg_revision STREQUAL expected_baseline)
        message(FATAL_ERROR
            "The vcpkg checkout is not pinned to vcpkg.json. Expected ${expected_baseline}, found "
            "${actual_vcpkg_revision}. Run 'git -C ${vcpkg_root} checkout --detach ${expected_baseline}'.")
    endif()

    execute_process(
        COMMAND git -C "${vcpkg_root}" status --porcelain --untracked-files=no
        RESULT_VARIABLE status_result
        OUTPUT_VARIABLE tracked_changes
        ERROR_VARIABLE status_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT status_result EQUAL 0)
        message(FATAL_ERROR
            "Could not verify the pinned vcpkg checkout state at '${vcpkg_root}': ${status_error}")
    endif()
    if(NOT tracked_changes STREQUAL "")
        message(FATAL_ERROR
            "The pinned vcpkg checkout has tracked modifications and cannot provide reproducible "
            "dependencies:\n${tracked_changes}\nUse a clean checkout at ${expected_baseline}.")
    endif()

    set(CCH_VCPKG_ROOT "${vcpkg_root}" CACHE INTERNAL "Validated pinned vcpkg root")
endfunction()

function(cch_validate_supported_build_after_project)
    if(NOT DEFINED CCH_TOOLCHAIN_ROLE)
        set(CCH_TOOLCHAIN_ROLE "build")
    endif()

    if(CMAKE_CROSSCOMPILING OR NOT "${CMAKE_SYSROOT}" STREQUAL "" OR
       NOT "${CMAKE_CXX_COMPILER_TARGET}" STREQUAL "")
        set(is_cross_build TRUE)
    else()
        set(is_cross_build FALSE)
    endif()

    cch_validate_compiler_role(
        "${CCH_TOOLCHAIN_ROLE}"
        "${CMAKE_CXX_COMPILER_ID}"
        "${CMAKE_CXX_COMPILER_VERSION}")
    cch_validate_configuration_role("${CCH_TOOLCHAIN_ROLE}" "${CMAKE_BUILD_TYPE}")

    execute_process(
        COMMAND "${CMAKE_MAKE_PROGRAM}" --version
        RESULT_VARIABLE ninja_result
        OUTPUT_VARIABLE ninja_version
        ERROR_VARIABLE ninja_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT ninja_result EQUAL 0 OR ninja_version STREQUAL "")
        message(FATAL_ERROR
            "Could not determine the configured Ninja version from '${CMAKE_MAKE_PROGRAM}': ${ninja_error}")
    endif()

    include(CheckCXXSourceCompiles)
    unset(CCH_TARGET_HAS_GLIBC CACHE)
    set(CMAKE_REQUIRED_QUIET TRUE)
    check_cxx_source_compiles(
        "#include <features.h>\n#ifndef __GLIBC__\n#error glibc required\n#endif\nint main() { return 0; }"
        CCH_TARGET_HAS_GLIBC)
    unset(CMAKE_REQUIRED_QUIET)

    cch_validate_platform(
        "${CMAKE_SYSTEM_NAME}"
        "${CMAKE_SYSTEM_PROCESSOR}"
        "${is_cross_build}"
        "${CMAKE_SYSROOT}"
        "${CMAKE_CXX_COMPILER_TARGET}"
        "${CCH_TARGET_HAS_GLIBC}")
    cch_validate_generator("${CMAKE_GENERATOR}" "${ninja_version}" "${CMAKE_UNITY_BUILD}")
    cch_validate_vcpkg_shape(
        "${CMAKE_TOOLCHAIN_FILE}"
        "${VCPKG_MANIFEST_MODE}"
        "${VCPKG_MANIFEST_INSTALL}"
        "${VCPKG_TARGET_TRIPLET}")
    cch_validate_vcpkg_options(
        "${VCPKG_HOST_TRIPLET}"
        "${VCPKG_OVERLAY_PORTS}"
        "${VCPKG_OVERLAY_TRIPLETS}"
        "${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")

    file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" source_dir)
    file(REAL_PATH "${VCPKG_MANIFEST_DIR}" manifest_dir)
    if(NOT manifest_dir STREQUAL source_dir)
        message(FATAL_ERROR
            "vcpkg manifest mode resolved '${VCPKG_MANIFEST_DIR}' instead of this source tree "
            "('${CMAKE_CURRENT_SOURCE_DIR}'). Dependency manifest fallback is unsupported.")
    endif()

    message(STATUS
        "Supported build: native Linux x86-64 glibc, ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} (${CCH_TOOLCHAIN_ROLE}), Ninja ${ninja_version}, pinned vcpkg")
endfunction()

function(cch_require_vcpkg_dependency dependency_name resolved_path)
    set(installed_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    cch_validate_vcpkg_dependency_path(
        "${dependency_name}"
        "${installed_prefix}"
        "${resolved_path}")
endfunction()
