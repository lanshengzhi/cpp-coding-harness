include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Independent durable runtime: the durable Session, the Storage contract and
# the memory backend (ADR 0075; issue #1063). It is a CONSUMER of the
# durable-required Chord subset (context values, strict JSON, immutable
# document revisions with delta preparation) and owns no application Session,
# CLI, or frontend integration: the ordinary coding-agent Session stays where
# it is.
#
# This slice ships the Session, the Storage contract and the JSONL and memory
# backends. SQLite is a later slice, and the durable task graph, submissions,
# documents-as-a-capability and rewindable history are out of scope here.
cch_parity_declare_target(
    TARGET cch_durable
    ROLE owner
    OWNER cch_durable
    SOURCES
        src/durable/JsonlStorage.cpp
        src/durable/Session.cpp
        src/durable/Storage.cpp
        src/durable/StorageState.cpp
    DEPENDS
        cch_chord
        cch_support
    INTERFACE_DEPENDS
        cch_chord
        cch_support
)
cch_owner_include_roots(cch_durable src/durable/include)
