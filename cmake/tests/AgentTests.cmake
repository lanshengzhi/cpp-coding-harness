include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # agent
    add_executable(cch_tests_agent
        tests/Catch2Main.cpp
        tests/agent/AgentCoreEvidenceTest.cpp
        tests/agent/AgentTest.cpp
        tests/agent/AgentBehaviorTest.cpp
        tests/agent/ModelRuntimeSeamTest.cpp
        tests/agent/ToolCallExecutorTest.cpp
        tests/agent/NestedToolCallsTest.cpp
        tests/agent/AgentToolSurfaceTest.cpp
)
    target_include_directories(cch_tests_agent PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_agent
        PRIVATE
            cch_agent_core
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_agent PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_agent PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_agent ADD_TAGS_AS_LABELS)
    add_test(NAME cch_agent_realtime_jump
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/agent/RealtimeJumpTest.py
                $<TARGET_FILE:cch_tests_agent> ${CMAKE_CXX_COMPILER})
    set_tests_properties(cch_agent_realtime_jump PROPERTIES LABELS "agent;issue1106;spec" TIMEOUT 30)
    add_dependencies(cch_tests_agent ${CCH_PARITY_BUILD_GATE_TARGET})
