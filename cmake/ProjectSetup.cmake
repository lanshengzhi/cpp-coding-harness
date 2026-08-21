include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Release-artifact IPO/LTO (ADR 0039; issue #474): the vcpkg-release-artifact
# preset opts in through CMAKE_INTERPROCEDURAL_OPTIMIZATION. Validate IPO at
# configure time so an unsupported toolchain fails closed instead of silently
# producing a non-LTO "release artifact".
if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT cch_ipo_supported OUTPUT cch_ipo_error LANGUAGES CXX)
    if(NOT cch_ipo_supported)
        message(FATAL_ERROR
            "IPO/LTO was requested but this toolchain does not support it: ${cch_ipo_error}")
    endif()
    message(STATUS "IPO/LTO enabled and validated for this configuration")
endif()

# CMake 3.30+ removed the legacy FindBoost module; use Boost's own config-mode package.
cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CCH_BUILD_TESTS "Build tests" ON)
option(CCH_STRICT_NO_EXCEPTIONS
    "Require -fno-exceptions and the strict exception allowlist"
    ON)
if(CCH_STRICT_NO_EXCEPTIONS)
    # The strict no-exception policy is the default build (ADR 0042): ordinary
    # failure is cch::support::Expected / std::error_code, and an exception is
    # not a second error channel. It applies to every project-owned C++ target,
    # including test shards. Disabling the option is a deliberate deviation
    # from the supported policy and is intended only for local debugging.
    add_compile_options(-fno-exceptions)
    # Boost.Asio's completion machinery must take the same no-exception path
    # as the project-owned code. The compiler flag also makes Boost.Config set
    # BOOST_NO_EXCEPTIONS, but this explicit definition keeps the Asio policy
    # visible in every strict compile command.
    add_compile_definitions(BOOST_ASIO_NO_EXCEPTIONS)
    message(STATUS "Strict no-exception policy enabled")
else()
    message(WARNING
        "CCH_STRICT_NO_EXCEPTIONS is OFF: the default strict no-exception "
        "policy (ADR 0042) is disabled for this configuration")
endif()

set(CCH_WARNING_OPTIONS -Wall -Wextra -Wpedantic)

# Sanitizer coverage (issue #473). The blocking sanitizer CI jobs configure
# through the checked-in vcpkg-asan-ubsan / vcpkg-tsan presets, which set this
# cache variable; validation is fail-closed so an unknown selection is a
# configure error, never a silently unsanitized build. Flags apply to every
# target declared below, production and test alike.
set(CCH_SANITIZER "" CACHE STRING "Sanitizer selection: empty, 'address;undefined', or 'thread'")
cch_validate_sanitizer_selection("${CCH_SANITIZER}")
if(CCH_SANITIZER)
    string(REPLACE ";" "," sanitizer_commas "${CCH_SANITIZER}")
    add_compile_options("-fsanitize=${sanitizer_commas}" -fno-omit-frame-pointer)
    add_link_options("-fsanitize=${sanitizer_commas}")
    message(STATUS "Sanitizer coverage: ${sanitizer_commas}")
endif()
