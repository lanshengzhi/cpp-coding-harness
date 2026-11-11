include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Reusable codemode sandbox, source grammar and declaration rendering (pi
# `packages/codemode`). It owns no application Session or tool-registry
# integration; coding-agent and durable call it directly. The source list is
# shared with the #1232 test-only variant in cmake/tests/CodemodeTests.cmake so
# the variant cannot silently drift from the production translation units.
set(CCH_CODEMODE_SOURCES
    src/codemode/Declarations.cpp
    src/codemode/Deterministic.cpp
    src/codemode/Guest.cpp
    src/codemode/Remote.cpp
    src/codemode/Sandbox.cpp
    src/codemode/Shared.cpp
    src/codemode/Source.cpp
)
cch_parity_declare_target(
    TARGET cch_codemode
    ROLE owner
    OWNER cch_codemode
    SOURCES ${CCH_CODEMODE_SOURCES}
    DEPENDS
        cch_ai
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        wasmedge@wasmedge
    INTERFACE_DEPENDS
        cch_ai
        cch_support
)
cch_owner_include_roots(cch_codemode src/codemode/include)
# The source-build guest fallback resolves against the checkout, exactly as
# the application's own source-tree fallback did before the extraction.
target_compile_definitions(cch_codemode PRIVATE
    CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# Portable no-Node entry (#1194): the source options prefix/grammar, the
# prelude and declaration constants, and the Remote protocol types, without
# the worker-mode engine's WasmEdge/Boost.Asio/thread dependencies. The
# application bridge stays in `cch_coding_agent`; the full engine stays in
# `cch_codemode` above.
cch_parity_declare_target(
    TARGET cch_codemode_portable
    ROLE owner
    OWNER cch_codemode
    SOURCES
        src/codemode/Portable.cpp
    DEPENDS
        cch_ai
        cch_support
    INTERFACE_DEPENDS
        cch_ai
        cch_support
)
cch_owner_include_roots(cch_codemode_portable src/codemode/include)
# Build-level no-Node/no-native check, not only a header-existence assertion:
# the portable target must not declare or link a native-only runtime. A target
# that secretly picks up WasmEdge, Boost or Threads fails the configure here,
# before any source-only constant check could pass.
foreach(_portable_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_portable_deps cch_codemode_portable ${_portable_property})
    if(_portable_deps)
        foreach(_native_token wasmedge Threads Boost boost asio)
            if("${_portable_deps}" MATCHES "${_native_token}")
                message(FATAL_ERROR
                    "cch_codemode_portable must stay a no-Node/no-native entry; "
                    "${_portable_property} declares a native-only dependency: ${_portable_deps}")
            endif()
        endforeach()
    endif()
endforeach()
