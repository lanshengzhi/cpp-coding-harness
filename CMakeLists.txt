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

find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(utf8proc CONFIG QUIET)
if(NOT TARGET utf8proc::utf8proc)
    find_path(UTF8PROC_INCLUDE_DIR NAMES utf8proc.h REQUIRED)
    find_library(UTF8PROC_LIBRARY NAMES utf8proc REQUIRED)
    add_library(utf8proc::utf8proc UNKNOWN IMPORTED)
    set_target_properties(utf8proc::utf8proc PROPERTIES
        IMPORTED_LOCATION "${UTF8PROC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${UTF8PROC_INCLUDE_DIR}"
    )
endif()

set(CCH_WARNING_OPTIONS -Wall -Wextra -Wpedantic)

function(cch_target_defaults target_name)
    target_include_directories(${target_name}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(${target_name} PRIVATE ${CCH_WARNING_OPTIONS})
endfunction()

add_library(cch_util
    src/util/Process.cpp
)
cch_target_defaults(cch_util)
target_link_libraries(cch_util
    PUBLIC
        Boost::filesystem
        Threads::Threads
    PRIVATE
        glaze::glaze
)

add_library(cch_tui
    src/tui/Container.cpp
    src/tui/Editor.cpp
    src/tui/Input.cpp
    src/tui/InputDecoder.cpp
    src/tui/Text.cpp
    src/tui/TruncatedText.cpp
    src/tui/Tui.cpp
    src/tui/UnicodeWidth.cpp
    src/tui/VirtualTerminal.cpp
)
cch_target_defaults(cch_tui)
target_link_libraries(cch_tui
    PRIVATE
        cch_util
        utf8proc::utf8proc
)

add_library(cch_ai
    src/ai/ProviderRegistry.cpp
    src/ai/providers/BoostBeastStreamTransport.cpp
    src/ai/providers/FakeChatClient.cpp
    src/ai/providers/OpenAIChatClient.cpp
    src/ai/providers/SseParser.cpp
)
cch_target_defaults(cch_ai)
target_link_libraries(cch_ai
    PUBLIC
        cch_util
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)
target_compile_options(cch_ai PRIVATE -Wall -Wextra -Wpedantic)

add_library(cch_agent
    src/agent/Agent.cpp
    src/agent/AgentLoop.cpp
    src/agent/AgentPolicyAdapters.cpp
    src/agent/ToolArgumentPreparation.cpp
    src/agent/ToolCallExecutor.cpp
)
cch_target_defaults(cch_agent)
target_link_libraries(cch_agent
    PUBLIC
        cch_ai
        cch_util
)

add_library(cch_harness
    src/harness/AsyncLocalExecutionEnv.cpp
    src/harness/SyncLocalExecutionEnv.cpp
    src/harness/WorkspaceFileSystemFdWalk.cpp
    src/harness/WorkspaceFileSystemLegacy.cpp
    src/harness/WorkspaceFileSystemPi.cpp
    src/harness/WorkspaceFileSystemTemp.cpp
    src/harness/session/SessionJournal.cpp
    src/harness/session/EntrySerializer.cpp
    src/harness/session/InMemorySessionStore.cpp
    src/harness/session/JsonlSessionStore.cpp
    src/harness/session/SessionResume.cpp
    src/harness/session/SessionTree.cpp
)
cch_target_defaults(cch_harness)
target_link_libraries(cch_harness
    PUBLIC
        cch_ai
        cch_util
)

add_library(cch_tools
    src/tools/AsyncToolFactories.cpp
)
cch_target_defaults(cch_tools)
target_link_libraries(cch_tools
    PUBLIC
        cch_agent
        cch_harness
        cch_ai
        cch_util
)

add_library(cch_coding_agent_runtime
    src/coding_agent/runtime/AsyncCliRuntime.cpp
    src/coding_agent/AgentConfigDir.cpp
    src/coding_agent/AuthLoader.cpp
    src/coding_agent/CommandRegistry.cpp
    src/coding_agent/SessionPathPolicy.cpp
    src/coding_agent/SettingsLoader.cpp
    src/coding_agent/ProviderConfigResolution.cpp
    src/coding_agent/ProjectResourceLoader.cpp
    src/coding_agent/ProjectResources.cpp
    src/coding_agent/ProjectTrust.cpp
    src/coding_agent/prompt/PromptProcessor.cpp
    src/coding_agent/prompt/PromptTemplateExpander.cpp
    src/coding_agent/prompt/SlashCommandParser.cpp
    src/coding_agent/Sdk.cpp
    src/coding_agent/SkillFrontmatterParser.cpp
    src/coding_agent/SkillLoader.cpp
    src/coding_agent/PromptTemplateLoader.cpp
    src/coding_agent/SkillFormatting.cpp
    src/coding_agent/runtime/AgentSessionRuntime.cpp
    src/coding_agent/runtime/EventPrinter.cpp
    src/coding_agent/runtime/SessionEventCommitment.cpp
    src/cli/InteractiveCliFrontend.cpp
    src/cli/JsonCliRenderer.cpp
    src/cli/TextCliRenderer.cpp
    src/coding_agent/runtime/SessionFactory.cpp
    src/coding_agent/runtime/JsonEventPrinter.cpp
    src/coding_agent/runtime/RpcJsonl.cpp
    src/coding_agent/runtime/RpcMode.cpp
    src/coding_agent/runtime/SessionLifecycle.cpp
)
cch_target_defaults(cch_coding_agent_runtime)
target_include_directories(cch_coding_agent_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(cch_coding_agent_runtime
    PUBLIC
        cch_agent
        cch_harness
        cch_tools
        cch_ai
        cch_util
)

# Transitional aggregate target for the executable/tests while the runtime is being split.
add_library(cpp_harness_lib INTERFACE)
target_link_libraries(cpp_harness_lib INTERFACE cch_coding_agent_runtime)

add_executable(cpp_harness
    src/main.cpp
    src/cli/CliParse.cpp
)
target_link_libraries(cpp_harness PRIVATE cpp_harness_lib CLI11::CLI11)
target_include_directories(cpp_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(cpp_harness PRIVATE ${CCH_WARNING_OPTIONS})

if(CCH_BUILD_TESTS)
    include(CTest)
    add_executable(cpp_harness_tests
        tests/TestMain.cpp
        tests/support/ComplexToolSchemaFixture.hpp
        tests/support/ToolArgumentCompatibilityFixture.hpp
        tests/architecture/ArchitectureSurfaceScanTest.cpp
        tests/architecture/CMakeDependencyTest.cpp
        tests/architecture/MoveOnlyCallbackTest.cpp
        tests/architecture/PublicHeaderBoundaryTest.cpp
        tests/agent/AgentTest.cpp
        tests/agent/AsyncAgentLoopTest.cpp
        tests/agent/ToolCallExecutorTest.cpp
        tests/ai/GlazeRoundTripTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/ProviderRegistryTest.cpp
        tests/ai/ToolContractTest.cpp
        tests/ai/providers/BoostBeastStreamTransportTest.cpp
        tests/ai/providers/FakeChatClientTest.cpp
        tests/ai/providers/OpenAIChatClientTest.cpp
        tests/ai/providers/SseParserTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/cli/CliParseTest.cpp
        tests/cli/InteractiveCliFrontendTest.cpp
        tests/coding_agent/BuiltinCommandsTest.cpp
        tests/coding_agent/CommandRegistryTest.cpp
        tests/coding_agent/PromptProcessorTest.cpp
        tests/coding_agent/PromptTemplateProcessingTest.cpp
        tests/coding_agent/AuthLoaderTest.cpp
        tests/coding_agent/AgentConfigDirTest.cpp
        tests/coding_agent/SessionPathPolicyTest.cpp
        tests/coding_agent/SettingsLoaderTest.cpp
        tests/coding_agent/ProviderConfigResolutionTest.cpp
        tests/coding_agent/ProjectResourcesTest.cpp
        tests/coding_agent/ProjectResourceLoaderTest.cpp
        tests/coding_agent/ProjectTrustTest.cpp
        tests/coding_agent/SkillTest.cpp
        tests/coding_agent/SkillFormattingTest.cpp
        tests/coding_agent/SkillFrontmatterParserTest.cpp
        tests/coding_agent/SkillLoaderTest.cpp
        tests/coding_agent/PromptTemplateLoaderTest.cpp
        tests/coding_agent/SdkSessionTest.cpp
        tests/coding_agent/AgentSessionSnapshotTest.cpp
        tests/coding_agent/SkillIntegrationTest.cpp
        tests/coding_agent/runtime/JsonEventPrinterTest.cpp
        tests/coding_agent/runtime/RpcModeTest.cpp
        tests/coding_agent/runtime/SessionEventCommitmentTest.cpp
        tests/coding_agent/runtime/SessionLifecycleTest.cpp
        tests/harness/AsyncLocalExecutionEnvTest.cpp
        tests/harness/session/InMemorySessionStoreTest.cpp
        tests/harness/session/JsonlSessionStoreTest.cpp
        tests/harness/session/SessionTreeTest.cpp
        tests/tools/AsyncToolsTest.cpp
        tests/tui/ContainerTest.cpp
        tests/tui/EditorTest.cpp
        tests/tui/InputTest.cpp
        tests/tui/TruncatedTextTest.cpp
        tests/tui/TuiTest.cpp
        tests/tui/UnicodeWidthTest.cpp
        tests/harness/WorkspaceFileSystemTest.cpp
        tests/util/BoundedTextTest.cpp
        tests/util/ExpectedErrorTest.cpp
        tests/util/ExpectedMacrosTest.cpp
        tests/util/OutputLimiterTest.cpp
        tests/util/RedactorTest.cpp
    )
    target_include_directories(cpp_harness_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        third_party
    )
    target_link_libraries(cpp_harness_tests PRIVATE cpp_harness_lib cch_tui CLI11::CLI11)
    target_sources(cpp_harness_tests PRIVATE
        src/cli/CliParse.cpp
    )
    target_compile_definitions(cpp_harness_tests PRIVATE
        CCH_BINARY="$<TARGET_FILE:cpp_harness>"
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(cpp_harness_tests PRIVATE ${CCH_WARNING_OPTIONS})
    add_dependencies(cpp_harness_tests cpp_harness)
    add_test(NAME cpp_harness_tests COMMAND cpp_harness_tests)
endif()
