include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Durable-required Chord surface (ADR 0075): immutable Context values with
# keyed lookup and composed cancellation, immutable document revisions with
# delta preparation/adoption/application, strict JSON validation and copying,
# and bounded UTF-8 output with exact dropped counts. Upstream Chord's plugin
# host, RPC links, cluster, cells/schema and Node host remain excluded; this
# package owns only the durable-required subset.
cch_parity_declare_target(
    TARGET cch_chord
    ROLE owner
    OWNER cch_chord
    SOURCES
        src/chord/ChordContext.cpp
        src/chord/Json.cpp
        src/chord/Delta.cpp
        src/chord/Tracker.cpp
        src/chord/StrictJson.cpp
        src/chord/Output.cpp
        src/chord/ReplicatedState.cpp
    DEPENDS
        cch_support
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_chord src/chord/include)
