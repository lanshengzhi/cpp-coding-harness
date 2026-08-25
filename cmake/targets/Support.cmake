include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Pi-neutral C++ Support Package (ADR 0039; #469). Owns passive Error/Expected,
# JsonValue (including its hand-rolled I/O), AsyncResult, and shared move-only
# mechanics used across Owners. It carries no product policy or process policy.
# Serialization DTOs, redaction, and output limiting remain with their
# Capability Owners unless multiple Owners genuinely require the same
# pi-neutral mechanic (ADR 0039): the bounded/redacted text mechanics and the
# no-exception Boost completion bridge live here on that clause (ADR 0046;
# #539).
cch_parity_declare_target(
    TARGET cch_support
    ROLE support
    OWNER cch_support
    SOURCES
        src/support/BoostExceptionHandler.cpp
        src/support/Json.cpp
)
cch_owner_include_roots(cch_support src/support/include)
# Boost's no-exception configuration leaves the application-owned
# boost::throw_exception termination hook to the consuming target. Compile the
# private hook without exceptions even if the strict policy is disabled locally
# (ADR 0042).
set_source_files_properties(src/support/BoostExceptionHandler.cpp PROPERTIES
    COMPILE_OPTIONS -fno-exceptions
    COMPILE_DEFINITIONS BOOST_ASIO_NO_EXCEPTIONS
)
