include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

cch_parity_declare_target(
    TARGET cch_agent_core
    ROLE owner
    OWNER cch_agent_core
    SOURCES
        src/agent/Agent.cpp
        src/agent/AgentPolicyAdapters.cpp
        src/agent/ToolArgumentPreparation.cpp
        src/agent/ToolCallExecutor.cpp
        src/agent/harness/AsyncLocalExecutionEnv.cpp
        src/agent/harness/AsyncLocalFileSystem.cpp
        src/agent/harness/Process.cpp
        src/agent/harness/RuntimeRoot.cpp
        src/agent/harness/ShellResolver.cpp
        src/agent/harness/SyncLocalExecutionEnv.cpp
        src/agent/harness/WorkspaceFileSystemFdWalk.cpp
        src/agent/harness/WorkspaceFileSystemLegacy.cpp
        src/agent/harness/WorkspaceFileSystemPi.cpp
        src/agent/harness/WorkspaceFileSystemTemp.cpp
        src/agent/harness/compaction/Compaction.cpp
        src/agent/harness/session/SessionJournal.cpp
        src/agent/harness/session/EntrySerializer.cpp
        src/agent/harness/session/InMemorySessionStore.cpp
        src/agent/harness/session/JsonlSessionStore.cpp
        src/agent/harness/session/SessionStore.cpp
        src/agent/harness/session/SessionResume.cpp
        src/agent/harness/session/SessionTree.cpp
        src/agent/tools/AsyncToolFactories.cpp
        src/agent/tools/EditDiff.cpp
    DEPENDS
        cch_ai
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
        utf8proc::utf8proc@utf8proc
    INTERFACE_DEPENDS
        cch_ai
        cch_support
)
cch_owner_include_roots(cch_agent_core src/agent/include)
