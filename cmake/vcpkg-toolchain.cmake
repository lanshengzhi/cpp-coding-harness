# Toolchain wrapper providing a default fallback to the in-tree .deps/vcpkg.
#
# If VCPKG_ROOT is defined in the environment (e.g. CI or a custom checkout),
# it takes precedence. Otherwise, defaults to ${CMAKE_CURRENT_LIST_DIR}/../.deps/vcpkg.
#
# Once resolved, CMAKE_TOOLCHAIN_FILE is updated to point directly to
# vcpkg.cmake so that all downstream validation (SupportedBuild.cmake)
# sees the canonical toolchain path and verified vcpkg root.

if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(_cch_vcpkg_root "$ENV{VCPKG_ROOT}")
else()
    cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH _cch_repo_root)
    set(_cch_vcpkg_root "${_cch_repo_root}/.deps/vcpkg")
endif()

if(NOT EXISTS "${_cch_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR
        "The pinned vcpkg toolchain was not found at '${_cch_vcpkg_root}/scripts/buildsystems/vcpkg.cmake'.\n"
        "Please run 'scripts/bootstrap.sh' to initialize .deps/vcpkg, or set the VCPKG_ROOT environment variable to your vcpkg checkout."
    )
endif()

if(NOT DEFINED ENV{VCPKG_ROOT} OR "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(ENV{VCPKG_ROOT} "${_cch_vcpkg_root}")
endif()

set(CMAKE_TOOLCHAIN_FILE "${_cch_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "The CMake toolchain file" FORCE)
set(CMAKE_TOOLCHAIN_FILE "${_cch_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")

include("${_cch_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
