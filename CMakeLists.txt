cmake_minimum_required(VERSION 3.20)
project(cpp_coding_harness VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CCH_BUILD_TESTS "Build tests" ON)

find_package(Boost REQUIRED COMPONENTS json system filesystem)
find_package(OpenSSL REQUIRED)

add_library(cpp_harness_lib
    src/agent/AgentLoop.cpp
    src/ai/providers/BoostBeastHttpTransport.cpp
    src/ai/providers/OpenAIChatClient.cpp
    src/llm/BoostBeastHttpTransport.cpp
    src/llm/OpenAIChatClient.cpp
    src/session/JsonlSessionStore.cpp
    src/tools/BashTool.cpp
    src/tools/EditFileTool.cpp
    src/tools/ReadFileTool.cpp
    src/tools/ToolFactories.cpp
    src/tools/WriteFileTool.cpp
)

target_include_directories(cpp_harness_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cpp_harness_lib
    PUBLIC
        Boost::json
        Boost::system
        Boost::filesystem
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_options(cpp_harness_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cpp_harness src/main.cpp)
target_link_libraries(cpp_harness PRIVATE cpp_harness_lib)
target_compile_options(cpp_harness PRIVATE -Wall -Wextra -Wpedantic)

if(CCH_BUILD_TESTS)
    include(CTest)
    add_executable(cpp_harness_tests
        tests/TestMain.cpp
        tests/agent/AgentEventTest.cpp
        tests/agent/AgentLoopTest.cpp
        tests/agent/AgentToolContractTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/ToolContractTest.cpp
        tests/ai/providers/OpenAIChatClientTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/llm/BoostBeastHttpTransportTest.cpp
        tests/llm/OpenAIChatClientTest.cpp
        tests/session/JsonlSessionStoreTest.cpp
        tests/tools/BashToolTest.cpp
        tests/tools/FileToolsTest.cpp
    )
    target_include_directories(cpp_harness_tests PRIVATE third_party)
    target_link_libraries(cpp_harness_tests PRIVATE cpp_harness_lib)
    target_compile_definitions(cpp_harness_tests PRIVATE CCH_BINARY="$<TARGET_FILE:cpp_harness>")
    target_compile_options(cpp_harness_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(cpp_harness_tests cpp_harness)
    add_test(NAME cpp_harness_tests COMMAND cpp_harness_tests)
endif()
