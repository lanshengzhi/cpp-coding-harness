include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Independent durable runtime: the durable Session, the Storage contract and
# the memory backend (ADR 0075; issue #1063). It is a CONSUMER of the
# durable-required Chord subset (context values, strict JSON, immutable
# document revisions with delta preparation) and owns no application Session,
# CLI, or frontend integration: the ordinary coding-agent Session stays where
# it is.
#
# The durable Storage backends include memory, JSONL and SQLite (#1075).
# #1079 adds a bounded model/tool workflow over the existing task runtime;
# AI streams come directly from cch_ai, with no agent/application dependency.
# Queued Submission storage and full task graph policies remain later slices.
cch_parity_declare_target(
    TARGET cch_durable
    ROLE owner
    OWNER cch_durable
    SOURCES
        src/durable/CodingTools.cpp
        src/durable/CommittedContext.cpp
        src/durable/Environment.cpp
        src/durable/Images.cpp
        src/durable/Compaction.cpp
        src/durable/Harness.cpp
        src/durable/JsonlStorage.cpp
        src/durable/Observation.cpp
        src/durable/SqliteStorage.cpp
        src/durable/Session.cpp
        src/durable/Storage.cpp
        src/durable/StorageEncoding.cpp
        src/durable/StorageState.cpp
        src/durable/Task.cpp
        src/durable/WorkflowRuntime.cpp
    DEPENDS
        Boost::headers@boost
        cch_ai
        cch_chord
        cch_support
        unofficial::sqlite3::sqlite3@sqlite3
        WebP::webpdecoder@webp
    INTERFACE_DEPENDS
        cch_ai
        cch_chord
        cch_support
)
cch_owner_include_roots(cch_durable src/durable/include)
# Vendored stb headers sit outside the project warning contract (ADR 0044);
# diagnostics they trigger are silenced at the inclusion site.
target_include_directories(cch_durable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
