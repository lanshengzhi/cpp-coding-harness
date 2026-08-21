include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Pi-neutral C++ Support Package (ADR 0039; #469). Owns passive Error/Expected,
# JsonValue (including its hand-rolled I/O), AsyncResult, and shared move-only
# mechanics used across Owners. It carries no product policy, process policy,
# serialization DTOs, redaction, or output-limiting policy; those remain with
# their Capability Owners.
cch_parity_declare_target(
    TARGET cch_support
    ROLE support
    OWNER cch_support
    SOURCES
        src/support/Json.cpp
)
cch_owner_include_roots(cch_support src/support/include)
