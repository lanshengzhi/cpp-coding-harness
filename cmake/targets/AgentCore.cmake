include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

cch_parity_declare_target(
    TARGET cch_agent_core
    ROLE owner
    OWNER cch_agent_core
    SOURCES
        src/agent/Agent.cpp
        src/agent/AgentRun.cpp
        src/agent/AgentSubscription.cpp
        src/agent/NestedToolCalls.cpp
        src/agent/ToolArgumentPreparation.cpp
        src/agent/ToolFormatValidation.cpp
        src/agent/ToolSchemaCompile.cpp
        src/agent/ToolSchemaValidate.cpp
        src/agent/ToolCallExecutor.cpp
        src/agent/ToolRegistry.cpp
    DEPENDS
        cch_ai
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
    INTERFACE_DEPENDS
        cch_ai
        cch_support
)
cch_owner_include_roots(cch_agent_core src/agent/include)
