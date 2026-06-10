cmake_minimum_required(VERSION 3.25)
project(cpp_coding_harness VERSION 0.1.0 LANGUAGES CXX)

# CMake 3.30+ removed the legacy FindBoost module; use Boost's own config-mode package.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CCH_BUILD_TESTS "Build tests" ON)

find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(OpenSSL REQUIRED)
find_package(glaze CONFIG REQUIRED)

add_library(cpp_harness_lib
    src/AsyncCliRuntime.cpp
    src/agent/AgentLoop.cpp
    src/ai/providers/BoostBeastStreamTransport.cpp
    src/ai/providers/OpenAIChatClient.cpp
    src/ai/providers/SseParser.cpp
    src/harness/AsyncLocalExecutionEnv.cpp
    src/harness/LocalExecutionEnv.cpp
    src/harness/session/JsonlSessionStore.cpp
    src/tools/AsyncToolFactories.cpp
)

target_include_directories(cpp_harness_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cpp_harness_lib
    PUBLIC
        glaze::glaze
        Boost::system
        Boost::filesystem
    PRIVATE
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
        tests/architecture/ArchitectureSurfaceScanTest.cpp
        tests/architecture/MoveOnlyCallbackTest.cpp
        tests/architecture/PublicHeaderBoundaryTest.cpp
        tests/agent/AsyncAgentLoopTest.cpp
        tests/ai/GlazeRoundTripTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/ToolContractTest.cpp
        tests/ai/providers/BoostBeastStreamTransportTest.cpp
        tests/ai/providers/OpenAIChatClientTest.cpp
        tests/ai/providers/SseParserTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/harness/AsyncLocalExecutionEnvTest.cpp
        tests/harness/session/JsonlSessionStoreTest.cpp
        tests/tools/AsyncToolsTest.cpp
        tests/util/ExpectedErrorTest.cpp
    )
    target_include_directories(cpp_harness_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        third_party
    )
    target_link_libraries(cpp_harness_tests PRIVATE cpp_harness_lib)
    target_compile_definitions(cpp_harness_tests PRIVATE
        CCH_BINARY="$<TARGET_FILE:cpp_harness>"
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(cpp_harness_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(cpp_harness_tests cpp_harness)
    add_test(NAME cpp_harness_tests COMMAND cpp_harness_tests)
endif()
