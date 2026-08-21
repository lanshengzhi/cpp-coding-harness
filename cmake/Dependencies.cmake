include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)

cch_require_vcpkg_dependency("Boost" "${Boost_DIR}")
cch_require_vcpkg_dependency("OpenSSL headers" "${OPENSSL_INCLUDE_DIR}")
cch_require_vcpkg_dependency("OpenSSL SSL library" "${OPENSSL_SSL_LIBRARY}")
cch_require_vcpkg_dependency("OpenSSL crypto library" "${OPENSSL_CRYPTO_LIBRARY}")
cch_require_vcpkg_dependency("glaze" "${glaze_DIR}")
cch_require_vcpkg_dependency("md4c" "${md4c_DIR}")
cch_require_vcpkg_dependency("WebP" "${WebP_DIR}")
cch_require_vcpkg_dependency("utf8proc" "${utf8proc_DIR}")

if(NOT TARGET WebP::webpdecoder)
    message(FATAL_ERROR "Pinned vcpkg WebP package does not provide WebP::webpdecoder")
endif()
if(NOT TARGET utf8proc::utf8proc)
    message(FATAL_ERROR "Pinned vcpkg utf8proc package does not provide utf8proc::utf8proc")
endif()
