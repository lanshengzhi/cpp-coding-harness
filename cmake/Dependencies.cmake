include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)

# WasmEdge (codemode sandbox, spec #865 / ticket #874). The pinned port installs
# only the C API headers and a static archive (no CMake config package), so the
# imported target is defined here. `wasmedge` is the parity-contract family for
# the runtime; the archive's own interface needs spdlog/fmt and the system
# runtime libraries, which stay inside this target and off the project surface.
#
# The optimized archive is used in every config. WasmEdge 0.13.5's Debug archive
# runs its interpreter unoptimized (its port builds Debug with no optimization),
# which is roughly two orders of magnitude too slow to evaluate pi's codemode
# prelude inside the lifecycle tests; the runtime is a self-contained
# third-party VM, so the config-specific Debug archive buys nothing here.
set(_cch_wasmedge_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(WASMEDGE_INCLUDE_DIR wasmedge/wasmedge.h)
set(WASMEDGE_LIBRARY "${_cch_wasmedge_prefix}/lib/libwasmedge.a")
set(WASMEDGE_SPDLOG_LIBRARY "${_cch_wasmedge_prefix}/lib/libspdlog.a")
set(WASMEDGE_FMT_LIBRARY "${_cch_wasmedge_prefix}/lib/libfmt.a")
foreach(_cch_wasmedge_archive "${WASMEDGE_LIBRARY}" "${WASMEDGE_SPDLOG_LIBRARY}" "${WASMEDGE_FMT_LIBRARY}")
    if(NOT EXISTS "${_cch_wasmedge_archive}")
        message(FATAL_ERROR "Pinned vcpkg WasmEdge closure is missing '${_cch_wasmedge_archive}'")
    endif()
endforeach()

cch_require_vcpkg_dependency("Boost" "${Boost_DIR}")
cch_require_vcpkg_dependency("OpenSSL headers" "${OPENSSL_INCLUDE_DIR}")
cch_require_vcpkg_dependency("OpenSSL SSL library" "${OPENSSL_SSL_LIBRARY}")
cch_require_vcpkg_dependency("OpenSSL crypto library" "${OPENSSL_CRYPTO_LIBRARY}")
cch_require_vcpkg_dependency("glaze" "${glaze_DIR}")
cch_require_vcpkg_dependency("md4c" "${md4c_DIR}")
cch_require_vcpkg_dependency("WebP" "${WebP_DIR}")
cch_require_vcpkg_dependency("utf8proc" "${utf8proc_DIR}")
cch_require_vcpkg_dependency("WasmEdge headers" "${WASMEDGE_INCLUDE_DIR}")
cch_require_vcpkg_dependency("WasmEdge library" "${WASMEDGE_LIBRARY}")
cch_require_vcpkg_dependency("spdlog library" "${WASMEDGE_SPDLOG_LIBRARY}")
cch_require_vcpkg_dependency("fmt library" "${WASMEDGE_FMT_LIBRARY}")

add_library(wasmedge STATIC IMPORTED GLOBAL)
set_target_properties(wasmedge PROPERTIES
    IMPORTED_LOCATION "${WASMEDGE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WASMEDGE_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES
        "${WASMEDGE_SPDLOG_LIBRARY};${WASMEDGE_FMT_LIBRARY};Threads::Threads;${CMAKE_DL_LIBS}")
if(NOT EXISTS "${WASMEDGE_LIBRARY}")
    message(FATAL_ERROR "Pinned vcpkg WasmEdge package did not resolve a static library")
endif()

if(NOT TARGET WebP::webpdecoder)
    message(FATAL_ERROR "Pinned vcpkg WebP package does not provide WebP::webpdecoder")
endif()
if(NOT TARGET utf8proc::utf8proc)
    message(FATAL_ERROR "Pinned vcpkg utf8proc package does not provide utf8proc::utf8proc")
endif()
