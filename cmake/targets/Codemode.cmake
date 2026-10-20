include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Reusable codemode sandbox, source grammar and declaration rendering (pi
# `packages/codemode`). It owns no application Session or tool-registry
# integration; coding-agent and durable call it directly.
cch_parity_declare_target(
    TARGET cch_codemode
    ROLE owner
    OWNER cch_codemode
    SOURCES
        src/codemode/Declarations.cpp
        src/codemode/Sandbox.cpp
        src/codemode/Source.cpp
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
