cmake_minimum_required(VERSION 3.20)
project(cpp_coding_harness VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CCH_BUILD_TESTS "Build tests" ON)

include(FetchContent)

find_package(Boost REQUIRED COMPONENTS json system filesystem)
find_package(OpenSSL REQUIRED)
find_package(glaze CONFIG QUIET)

if(NOT glaze_FOUND)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glaze)
endif()

add_library(cpp_harness_lib
    src/agent/AgentLoop.cpp
    src/agent/AsyncAgentLoop.cpp
    src/ai/providers/BoostBeastHttpTransport.cpp
    src/ai/providers/BoostBeastStreamTransport.cpp
    src/ai/providers/OpenAIChatClient.cpp
    src/ai/providers/OpenAIStreamingChatClient.cpp
    src/ai/providers/SseParser.cpp
    src/harness/LocalExecutionEnv.cpp
    src/harness/session/JsonlSessionStore.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cpp_harness_lib
    PUBLIC
        glaze::glaze
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
        tests/agent/AsyncAgentLoopTest.cpp
        tests/ai/GlazeRoundTripTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/ToolContractTest.cpp
        tests/ai/providers/BoostBeastStreamTransportTest.cpp
        tests/ai/providers/OpenAIChatClientTest.cpp
        tests/ai/providers/OpenAIStreamingChatClientTest.cpp
        tests/ai/providers/SseParserTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/harness/LocalExecutionEnvTest.cpp
        tests/harness/session/JsonlSessionStoreTest.cpp
        tests/llm/BoostBeastHttpTransportTest.cpp
        tests/llm/OpenAIChatClientTest.cpp
        tests/session/JsonlSessionStoreTest.cpp
        tests/tools/BashToolTest.cpp
        tests/tools/FileToolsTest.cpp
        tests/util/ExpectedErrorTest.cpp
    )
    target_include_directories(cpp_harness_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        third_party
    )
    target_link_libraries(cpp_harness_tests PRIVATE cpp_harness_lib)
    target_compile_definitions(cpp_harness_tests PRIVATE CCH_BINARY="$<TARGET_FILE:cpp_harness>")
    target_compile_options(cpp_harness_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(cpp_harness_tests cpp_harness)
    add_test(NAME cpp_harness_tests COMMAND cpp_harness_tests)
endif()
