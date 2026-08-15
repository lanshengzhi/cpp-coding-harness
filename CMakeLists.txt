cmake_minimum_required(VERSION 4.4)

set(CCH_TOOLCHAIN_ROLE "build" CACHE STRING "Supported compiler role: build or conformance")
set_property(CACHE CCH_TOOLCHAIN_ROLE PROPERTY STRINGS build conformance)

include(cmake/SupportedBuild.cmake)
cch_validate_supported_build_before_project()

project(cpp_coding_harness VERSION 0.1.0 LANGUAGES CXX)
cch_validate_supported_build_after_project()

# CMake 3.30+ removed the legacy FindBoost module; use Boost's own config-mode package.
cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CCH_BUILD_TESTS "Build tests" ON)

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)

cch_require_vcpkg_dependency("Boost" "${Boost_DIR}")
cch_require_vcpkg_dependency("OpenSSL headers" "${OPENSSL_INCLUDE_DIR}")
cch_require_vcpkg_dependency("OpenSSL SSL library" "${OPENSSL_SSL_LIBRARY}")
cch_require_vcpkg_dependency("OpenSSL crypto library" "${OPENSSL_CRYPTO_LIBRARY}")
cch_require_vcpkg_dependency("CLI11" "${CLI11_DIR}")
cch_require_vcpkg_dependency("glaze" "${glaze_DIR}")
cch_require_vcpkg_dependency("md4c" "${md4c_DIR}")
cch_require_vcpkg_dependency("WebP" "${WebP_DIR}")
cch_require_vcpkg_dependency("utf8proc" "${utf8proc_DIR}")

if(NOT TARGET WebP::webpdecoder)
    message(FATAL_ERROR "Pinned vcpkg WebP package does not provide WebP::webpdecoder")
endif()
if(NOT TARGET utf8proc::utf8proc)
    message(FATAL_ERROR "Pinned vcpkg utf8proc package does not provide utf8proc::utf8proc")
endif()

set(CCH_WARNING_OPTIONS -Wall -Wextra -Wpedantic)

# Central Parity Architecture constructors (ADR 0039). Every production target
# is declared through cch_parity_declare_target so its role, Owner, source
# ownership, direct dependencies, interface visibility, and external-family
# classification are recorded as configured evidence and emitted as the
# resolved ownership index. The manifest is the strict policy authority the
# Gate validates that evidence against.
include(cmake/ParityArchitecture.cmake)
cch_parity_set_manifest("${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json")

# Canonical repository-internal include surface: the global include root is
# published to dependents, the private src root stays private. Applied after
# each central declaration because the constructor creates the target but does
# not guess the include surface (Owner-local roots replace this in the
# contraction tickets).
function(cch_target_defaults target_name)
    target_include_directories(${target_name}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endfunction()

# Pi-neutral C++ Support Package (ADR 0039). Owns passive Error/Expected,
# JsonValue, and AsyncResult mechanics shared across Owners; header-only, so it
# has no compiled source of its own. The historical cch_util target below links
# it and remains the temporary legacy alias until the support contraction
# ticket #469 deletes it.
cch_parity_declare_target(
    TARGET cch_support
    ROLE support
    OWNER cch_support
    KIND interface
)
target_include_directories(cch_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

cch_parity_declare_target(
    TARGET cch_util
    ROLE support
    OWNER cch_support
    SOURCES
        src/util/Json.cpp
        src/util/Process.cpp
    DEPENDS
        cch_support
        Boost::filesystem@boost
        Threads::Threads@threads
    INTERFACE_DEPENDS
        cch_support
        Boost::filesystem@boost
        Threads::Threads@threads
)
cch_target_defaults(cch_util)

cch_parity_declare_target(
    TARGET cch_tui
    ROLE owner
    OWNER cch_tui
    SOURCES
        src/tui/Autocomplete.cpp
        src/tui/CancellableLoader.cpp
        src/tui/Container.cpp
        src/tui/Editor.cpp
        src/tui/Fuzzy.cpp
        src/tui/Image.cpp
        src/tui/Input.cpp
        src/tui/InputDecoder.cpp
        src/tui/Keybindings.cpp
        src/tui/KeyboardProtocol.cpp
        src/tui/Keys.cpp
        src/tui/Loader.cpp
        src/tui/Markdown.cpp
        src/tui/Overlay.cpp
        src/tui/ProcessTerminal.cpp
        src/tui/SelectList.cpp
        src/tui/SettingsList.cpp
        src/tui/TerminalImage.cpp
        src/tui/Text.cpp
        src/tui/TruncatedText.cpp
        src/tui/Tui.cpp
        src/tui/UnicodeWidth.cpp
        src/tui/Utils.cpp
        src/tui/VirtualTerminal.cpp
    DEPENDS
        cch_support
        cch_util
        md4c::md4c@md4c
        utf8proc::utf8proc@utf8proc
    INTERFACE_DEPENDS
        cch_support
)
cch_target_defaults(cch_tui)

cch_parity_declare_target(
    TARGET cch_coding_agent_tui
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/tui/KeybindingsManager.cpp
        src/coding_agent/tui/Theme.cpp
        src/coding_agent/tui/ThemeController.cpp
    DEPENDS
        cch_tui
        cch_util
)
cch_target_defaults(cch_coding_agent_tui)

cch_parity_declare_target(
    TARGET cch_ai
    ROLE owner
    OWNER cch_ai
    SOURCES
        src/ai/ContentUtil.cpp
        src/ai/Model.cpp
        src/ai/Models.cpp
        src/ai/ModelStream.cpp
        src/ai/SimpleOptions.cpp
        src/ai/Usage.cpp
        src/ai/api/AnthropicMessagesAdapter.cpp
        src/ai/api/MessageConversion.cpp
        src/ai/api/OpenAICodexResponsesAdapter.cpp
        src/ai/api/OpenAIResponsesAdapter.cpp
        src/ai/api/Termination.cpp
        src/ai/api/UsageNormalization.cpp
        src/ai/auth/OAuthCallbackServer.cpp
        src/ai/auth/OAuthHttpClient.cpp
        src/ai/auth/OpenAICodexOAuth.cpp
        src/ai/auth/KimiCodingOAuth.cpp
        src/ai/auth/OauthPage.cpp
        src/ai/auth/Pkce.cpp
        src/ai/providers/BoostBeastStreamTransport.cpp
        src/ai/providers/BoostBeastWebSocketTransport.cpp
        src/ai/providers/CodexCatalog.cpp
        src/ai/providers/EnvApiKeyAuth.cpp
        src/ai/providers/FakeProvider.cpp
        src/ai/providers/KimiCatalog.cpp
        src/ai/providers/KimiProvider.cpp
        src/ai/providers/RetryPolicy.cpp
        src/ai/providers/SseParser.cpp
        src/ai/utils/RetryClassifier.cpp
    DEPENDS
        cch_support
        cch_util
        OpenSSL::SSL@openssl
        OpenSSL::Crypto@openssl
    INTERFACE_DEPENDS
        cch_support
)
cch_target_defaults(cch_ai)

# ModelRuntime closure (Model/Models composition + Agent Config Directory + auth
# storage). The stateful Agent no longer depends on the coding-agent ModelRuntime
# seam: it consumes the AI-owned move-only ModelStream (ADR 0040 / #453), so the
# former reverse-DAG edge cch_agent -> cch_coding_agent_core is removed (#456).
cch_parity_declare_target(
    TARGET cch_coding_agent_core
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/ModelRuntime.cpp
        src/coding_agent/ModelResolver.cpp
        src/coding_agent/ModelConfig.cpp
        src/coding_agent/ProviderComposer.cpp
        src/coding_agent/AgentConfigDir.cpp
        src/coding_agent/AuthStorage.cpp
        src/coding_agent/RuntimeApiKeyOverlay.cpp
    DEPENDS
        cch_ai
        cch_util
    INTERFACE_DEPENDS
        cch_ai
        cch_util
)
cch_target_defaults(cch_coding_agent_core)

cch_parity_declare_target(
    TARGET cch_agent_core
    ROLE owner
    OWNER cch_agent_core
    SOURCES
        src/agent/Agent.cpp
        src/agent/AgentLoop.cpp
        src/agent/AgentPolicyAdapters.cpp
        src/agent/ToolArgumentPreparation.cpp
        src/agent/ToolCallExecutor.cpp
        src/harness/AsyncLocalExecutionEnv.cpp
        src/harness/RuntimeRoot.cpp
        src/harness/ShellResolver.cpp
        src/harness/SyncLocalExecutionEnv.cpp
        src/harness/WorkspaceFileSystemFdWalk.cpp
        src/harness/WorkspaceFileSystemLegacy.cpp
        src/harness/WorkspaceFileSystemPi.cpp
        src/harness/WorkspaceFileSystemTemp.cpp
        src/harness/compaction/Compaction.cpp
        src/harness/session/SessionJournal.cpp
        src/harness/session/EntrySerializer.cpp
        src/harness/session/InMemorySessionStore.cpp
        src/harness/session/JsonlSessionStore.cpp
        src/harness/session/SessionStore.cpp
        src/harness/session/SessionResume.cpp
        src/harness/session/SessionTree.cpp
        src/tools/AsyncToolFactories.cpp
        src/tools/EditDiff.cpp
    DEPENDS
        cch_ai
        cch_support
        cch_util
        utf8proc::utf8proc@utf8proc
    INTERFACE_DEPENDS
        cch_ai
        cch_support
)
cch_target_defaults(cch_agent_core)

cch_parity_declare_target(
    TARGET cch_coding_agent_runtime
    ROLE composition
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/ImageInput.cpp
        src/cli/InitialPrompt.cpp
        src/coding_agent/SessionDiscovery.cpp
        src/coding_agent/SessionPathPolicy.cpp
        src/coding_agent/SettingsManager.cpp
        src/coding_agent/ProjectResourceLoader.cpp
        src/coding_agent/ProjectResources.cpp
        src/coding_agent/ProjectTrust.cpp
        src/coding_agent/prompt/SystemPromptBuilder.cpp
        src/coding_agent/prompt/PromptExpansion.cpp
        src/coding_agent/prompt/PromptTemplateExpander.cpp
        src/coding_agent/prompt/BuiltinSlashCommands.cpp
        src/coding_agent/AgentSession.cpp
        src/coding_agent/SkillFrontmatterParser.cpp
        src/coding_agent/SkillLoader.cpp
        src/coding_agent/GitIgnoreMatcher.cpp
        src/coding_agent/PromptTemplateLoader.cpp
        src/coding_agent/SkillFormatting.cpp
        src/coding_agent/runtime/AgentSessionRuntime.cpp
        src/coding_agent/runtime/AuthGuidanceStream.cpp
        src/coding_agent/runtime/LocalUserShell.cpp
        src/coding_agent/runtime/SessionEventCommitment.cpp
        src/coding_agent/runtime/SessionPersistence.cpp
        src/coding_agent/runtime/UserBashOutputAccumulator.cpp
        src/cli/PrintMode.cpp
        src/cli/SessionFamily.cpp
        src/coding_agent/runtime/SessionFactory.cpp
        src/coding_agent/runtime/SessionFork.cpp
        src/coding_agent/runtime/SessionLifecycle.cpp
    DEPENDS
        cch_agent_core
        cch_coding_agent_core
        cch_ai
        cch_util
        WebP::webpdecoder@webp
    INTERFACE_DEPENDS
        cch_agent_core
        cch_coding_agent_core
        cch_ai
        cch_util
)
cch_target_defaults(cch_coding_agent_runtime)
target_include_directories(cch_coding_agent_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)
# The System Prompt's identity-adjusted documentation block resolves the C++
# binary's own docs paths from the source tree (pi `config.ts`
# getReadmePath/getDocsPath/getExamplesPath resolve the pi package).
target_compile_definitions(cch_coding_agent_runtime PRIVATE
    CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

cch_parity_declare_target(
    TARGET cch_coding_agent_interactive
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/tui/InteractiveMode.cpp
        src/coding_agent/tui/AssistantMessageComponent.cpp
        src/coding_agent/tui/BashExecutionComponent.cpp
        src/coding_agent/tui/ChatContainer.cpp
        src/coding_agent/tui/ClipboardWrite.cpp
        src/coding_agent/tui/DiffRenderer.cpp
        src/coding_agent/tui/ExternalEditor.cpp
        src/coding_agent/tui/Footer.cpp
        src/coding_agent/tui/FooterDataProvider.cpp
        src/coding_agent/tui/KeybindingHints.cpp
        src/coding_agent/tui/LoadedResources.cpp
        src/coding_agent/tui/LoginDialog.cpp
        src/coding_agent/tui/ReloadBox.cpp
        src/coding_agent/tui/LoginPresentation.cpp
        src/coding_agent/tui/ModelSelector.cpp
        src/coding_agent/tui/OAuthSelector.cpp
        src/coding_agent/tui/OpenBrowser.cpp
        src/coding_agent/tui/ScopedModelsSelector.cpp
        src/coding_agent/tui/SettingsSelector.cpp
        src/coding_agent/tui/SessionSelector.cpp
        src/coding_agent/tui/SessionSelectorSearch.cpp
        src/coding_agent/tui/StatusIndicator.cpp
        src/coding_agent/tui/StringListSelector.cpp
        src/coding_agent/tui/ToolExecutionComponent.cpp
        src/coding_agent/tui/TreeSelector.cpp
        src/coding_agent/tui/UserMessageComponent.cpp
        src/coding_agent/tui/UserMessageSelector.cpp
    DEPENDS
        cch_coding_agent_runtime
        cch_coding_agent_tui
        cch_tui
        cch_util
)
cch_target_defaults(cch_coding_agent_interactive)

# One authoritative CMake owner for the CLI/runtime composition shared by the
# executable and the tests (build-performance-plan Stage 3, item 2). Sits above
# the private interactive composition so the non-interactive runtime target
# keeps no TUI dependency.
cch_parity_declare_target(
    TARGET cch_cli
    ROLE composition
    OWNER cch_coding_agent
    SOURCES
        src/cli/CliParse.cpp
        src/cli/FrontendSelection.cpp
        src/cli/ListModels.cpp
        src/cli/StartupTui.cpp
        src/coding_agent/runtime/AsyncCliRuntime.cpp
    DEPENDS
        cch_coding_agent_interactive
        CLI11::CLI11@cli11
)
cch_target_defaults(cch_cli)
target_compile_definitions(cch_cli PRIVATE CCH_PROJECT_VERSION="${PROJECT_VERSION}")

# Transitional aggregate target for the executable/tests while the runtime is being split.
cch_parity_declare_target(
    TARGET cpp_harness_lib
    ROLE composition
    OWNER cch_coding_agent
    KIND interface
    DEPENDS
        cch_coding_agent_runtime
)

cch_parity_declare_target(
    TARGET cpp_harness
    ROLE composition
    OWNER cch_coding_agent
    KIND executable
    SOURCES
        src/main.cpp
    DEPENDS
        cpp_harness_lib
        cch_cli
)
target_include_directories(cpp_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# TEMPORARY non-blocking production audit (see #450; mandatory enforcement and
# deletion land in #470). Reports the current migration violations with stable
# rule IDs without failing configure, so the per-Owner contraction tickets can
# resolve them while the build stays green.
cch_parity_run_gate_report(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(CCH_BUILD_TESTS)
    # Suite-wide default hard timeout (ADR 0039; issue #447). CTest kills any
    # test that exceeds it, so a hang in a cancellation, Close, deadlock, or
    # fatal-contract case cannot stall the suite. The [fatal]-selected shard
    # discovery below tightens this further for the fatal probes.
    set(DART_TESTING_TIMEOUT 300 CACHE STRING
        "Default hard timeout (seconds) for every CTest test")
    include(CTest)

    find_package(Catch2 3 CONFIG REQUIRED)
    cch_require_vcpkg_dependency("Catch2" "${Catch2_DIR}")
    include(Catch)

    function(cch_add_supported_build_policy_test case_name should_pass diagnostic_pattern)
        set(test_name "cch_supported_build_policy_${case_name}")
        add_test(
            NAME ${test_name}
            COMMAND
                ${CMAKE_COMMAND}
                -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DCCH_TEST_CASE=${case_name}
                -DCCH_CASE_SHOULD_PASS=${should_pass}
                "-DCCH_DIAGNOSTIC_PATTERN=${diagnostic_pattern}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/SupportedBuildPolicyTest.cmake
        )
        set_tests_properties(${test_name} PROPERTIES LABELS "architecture;build;issue441")
    endfunction()

    cch_add_supported_build_policy_test("accept-gcc" TRUE "")
    cch_add_supported_build_policy_test("accept-clang" TRUE "")
    cch_add_supported_build_policy_test("reject-cross-build" FALSE "Cross-compilation is unsupported")
    cch_add_supported_build_policy_test("reject-operating-system" FALSE "native Linux")
    cch_add_supported_build_policy_test("reject-architecture" FALSE "x86-64")
    cch_add_supported_build_policy_test("reject-libc" FALSE "glibc")
    cch_add_supported_build_policy_test("reject-generator" FALSE "Ninja generator")
    cch_add_supported_build_policy_test("reject-ninja-version" FALSE "Ninja 1.11 or newer")
    cch_add_supported_build_policy_test("reject-unity" FALSE "Unity Build")
    cch_add_supported_build_policy_test("reject-gcc-version" FALSE "GCC 16.x")
    cch_add_supported_build_policy_test("reject-clang-version" FALSE "Clang 22.x")
    cch_add_supported_build_policy_test("reject-clang-build-role" FALSE "build role requires GCC 16.x")
    cch_add_supported_build_policy_test("reject-clang-release" FALSE "requires a Debug configuration")
    cch_add_supported_build_policy_test("reject-dependency-fallback" FALSE "outside the pinned vcpkg prefix")
    cch_add_supported_build_policy_test("reject-missing-vcpkg-toolchain" FALSE "pinned vcpkg toolchain")
    cch_add_supported_build_policy_test("reject-vcpkg-manifest-install" FALSE "manifest installation is required")
    cch_add_supported_build_policy_test("reject-vcpkg-host-triplet" FALSE "host triplet")
    cch_add_supported_build_policy_test("reject-vcpkg-overlays" FALSE "overlay ports")

    # Parity Architecture Gate (ADR 0039). The Python validator uses only the
    # standard library and Python 3.12+; it is a system tool, not a vcpkg
    # dependency. The unit case exercises the strict manifest, the
    # direct-include lexer and canonical resolution, compile-context, and
    # depfile-evidence rules directly; the fixture case configures a minimal
    # legal fixture, an illegal cross-Owner target-edge fixture, and a set of
    # poison-source fixtures end to end and asserts the stable rule-ID
    # rejections (PARITY-2001, PARITY-4xxx, PARITY-6001).
    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME cch_parity_gate_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/parity_gate_test.py
    )
    set_tests_properties(cch_parity_gate_unit PROPERTIES LABELS "architecture;parity-gate;issue448;issue449")

    add_test(
        NAME cch_parity_gate_fixture
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/ParityGateTest.cmake
    )
    set_tests_properties(cch_parity_gate_fixture PROPERTIES LABELS "architecture;parity-gate;issue448;issue449")

    # Package-aligned test shards (build-performance-plan Stage 4). Each shard
    # is its own executable registered with ctest, so a focused test edit
    # builds and links only its owning package. `cpp_harness_tests` remains as
    # the aggregate target that builds every shard. Grouping follows the
    # package boundaries in CODING_STANDARDS.md section 12 and fixture
    # ownership, not equal file counts; no test object is compiled twice.
    set(CCH_FORMAL_TEST_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

    # util
    add_executable(cch_tests_util
        tests/Catch2Main.cpp
        tests/util/BoundedTextTest.cpp
        tests/util/ExpectedErrorTest.cpp
        tests/util/ExpectedMacrosTest.cpp
        tests/util/JsonValueIoTest.cpp
        tests/util/OutputLimiterTest.cpp
        tests/util/ProcessTest.cpp
        tests/util/RedactorTest.cpp
        tests/util/TestRunnerIsolationTest.cpp
)
    target_include_directories(cch_tests_util PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_util
        PRIVATE
            cch_util
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_util PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_util PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_util ADD_TAGS_AS_LABELS)

    # support
    add_executable(cch_tests_support
        tests/Catch2Main.cpp
        tests/support/AllocationCounter.cpp
        tests/support/AsyncResultTest.cpp
)
    target_include_directories(cch_tests_support PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_support
        PRIVATE
            cch_support
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_support PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_support PRIVATE ${CCH_WARNING_OPTIONS})
    # Fatal-contract probes (tagged [fatal]) fork a child that must die from
    # SIGABRT. A hang means the probe broke, so they get a tightened hard
    # timeout instead of waiting out the suite-wide default (ADR 0039; #447).
    catch_discover_tests(cch_tests_support TEST_SPEC "~[fatal]" ADD_TAGS_AS_LABELS)
    catch_discover_tests(cch_tests_support TEST_SPEC "[fatal]" PROPERTIES TIMEOUT 60 ADD_TAGS_AS_LABELS)

    # TUI
    add_executable(cch_tests_tui
        tests/Catch2Main.cpp
        tests/tui/AutocompleteTest.cpp
        tests/tui/ContainerTest.cpp
        tests/tui/EditorTest.cpp
        tests/tui/FuzzyTest.cpp
        tests/tui/ImageTest.cpp
        tests/tui/InputTest.cpp
        tests/tui/KeybindingsTest.cpp
        tests/tui/KeysTest.cpp
        tests/tui/LoaderTest.cpp
        tests/tui/MarkdownTest.cpp
        tests/tui/OverlayTest.cpp
        tests/tui/PiTuiDifferentialTest.cpp
        tests/tui/ProcessTerminalTest.cpp
        tests/tui/RenderDifferentialTest.cpp
        tests/tui/ScreenStateGoldenTest.cpp
        tests/tui/SelectListTest.cpp
        tests/tui/SettingsListTest.cpp
        tests/tui/TerminalImageTest.cpp
        tests/tui/TruncatedTextTest.cpp
        tests/tui/TuiTest.cpp
        tests/tui/UnicodeWidthTest.cpp
        tests/tui/UtilsTest.cpp
)
    target_include_directories(cch_tests_tui PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_tui
        PRIVATE
            cch_tui
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_tui PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_tui PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_tui ADD_TAGS_AS_LABELS)

    # AI
    add_executable(cch_tests_ai
        tests/Catch2Main.cpp
        tests/ai/GlazeRoundTripTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/MessageConversionTest.cpp
        tests/ai/ModelTest.cpp
        tests/ai/ModelsTest.cpp
        tests/ai/ProviderPolicyTest.cpp
        tests/ai/RetryClassifierTest.cpp
        tests/ai/SimpleOptionsTest.cpp
        tests/ai/ToolContractTest.cpp
        tests/ai/UsageTest.cpp
        tests/ai/api/AnthropicMessagesAdapterTest.cpp
        tests/ai/api/OpenAICodexResponsesAdapterTest.cpp
        tests/ai/api/OpenAIResponsesAdapterTest.cpp
        tests/ai/api/PartialJsonTest.cpp
        tests/ai/auth/KimiCodingOAuthTest.cpp
        tests/ai/auth/OpenAICodexOAuthTest.cpp
        tests/ai/providers/BoostBeastStreamTransportTest.cpp
        tests/ai/providers/BoostBeastWebSocketTransportTest.cpp
        tests/ai/providers/FakeProviderTest.cpp
        tests/ai/providers/SseParserTest.cpp
)
    target_include_directories(cch_tests_ai PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_ai
        PRIVATE
            cch_agent_core
            cch_ai
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_ai PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_ai PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_ai ADD_TAGS_AS_LABELS)

    # agent
    add_executable(cch_tests_agent
        tests/Catch2Main.cpp
        tests/agent/AgentCoreEvidenceTest.cpp
        tests/agent/AgentTest.cpp
        tests/agent/AsyncAgentLoopTest.cpp
        tests/agent/ModelRuntimeSeamTest.cpp
        tests/agent/ToolCallExecutorTest.cpp
)
    target_include_directories(cch_tests_agent PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_agent
        PRIVATE
            cch_agent_core
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_agent PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_agent PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_agent ADD_TAGS_AS_LABELS)

    # harness and tools
    add_executable(cch_tests_harness_tools
        tests/Catch2Main.cpp
        tests/harness/AsyncLocalExecutionEnvTest.cpp
        tests/harness/RuntimeRootTest.cpp
        tests/harness/WorkspaceFileSystemTest.cpp
        tests/harness/compaction/CompactionTest.cpp
        tests/harness/session/InMemorySessionStoreTest.cpp
        tests/harness/session/JsonlSessionStoreTest.cpp
        tests/harness/session/SessionRoundTripGoldenTest.cpp
        tests/harness/session/SessionStoreTest.cpp
        tests/harness/session/SessionTreeTest.cpp
        tests/tools/AsyncToolsTest.cpp
)
    target_include_directories(cch_tests_harness_tools PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_harness_tools
        PRIVATE
            cch_agent_core
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_harness_tools PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_harness_tools PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_harness_tools ADD_TAGS_AS_LABELS)

    # coding-agent core/runtime
    add_executable(cch_tests_coding_agent
        tests/Catch2Main.cpp
        tests/coding_agent/AgentConfigDirTest.cpp
        tests/coding_agent/AgentSessionCompactionTest.cpp
        tests/coding_agent/AgentSessionSnapshotTest.cpp
        tests/coding_agent/AuthStorageTest.cpp
        tests/coding_agent/BuiltinSlashCommandsTest.cpp
        tests/coding_agent/ImageInputTest.cpp
        tests/coding_agent/KimiOAuthLifecycleTest.cpp
        tests/coding_agent/ModelConfigTest.cpp
        tests/coding_agent/ModelCycleTest.cpp
        tests/coding_agent/ModelResolutionTest.cpp
        tests/coding_agent/ModelResolverTest.cpp
        tests/coding_agent/ModelRuntimeTest.cpp
        tests/coding_agent/ProjectResourceLoaderTest.cpp
        tests/coding_agent/ProjectResourcesTest.cpp
        tests/coding_agent/ProjectTrustTest.cpp
        tests/coding_agent/PromptExpansionTest.cpp
        tests/coding_agent/PromptTemplateLoaderTest.cpp
        tests/coding_agent/PromptTemplateProcessingTest.cpp
        tests/coding_agent/ProviderComposerTest.cpp
        tests/coding_agent/ReAuthGuidanceTest.cpp
        tests/coding_agent/SessionDiscoveryTest.cpp
        tests/coding_agent/SessionPathPolicyTest.cpp
        tests/coding_agent/SetModelTest.cpp
        tests/coding_agent/SettingsManagerTest.cpp
        tests/coding_agent/SkillFormattingTest.cpp
        tests/coding_agent/SkillFrontmatterParserTest.cpp
        tests/coding_agent/SkillIntegrationTest.cpp
        tests/coding_agent/SkillLoaderTest.cpp
        tests/coding_agent/SkillTest.cpp
        tests/coding_agent/SystemPromptBuilderTest.cpp
        tests/coding_agent/SystemPromptFlowTest.cpp
        tests/coding_agent/SystemPromptGoldenTest.cpp
        tests/coding_agent/TurnAutoRetryTest.cpp
        tests/coding_agent/runtime/LocalUserShellTest.cpp
        tests/coding_agent/runtime/ResourceReloadTest.cpp
        tests/coding_agent/runtime/SessionCloseTest.cpp
        tests/coding_agent/runtime/SessionCommitmentAsyncTest.cpp
        tests/coding_agent/runtime/SessionEventCommitmentTest.cpp
        tests/coding_agent/runtime/SessionFactoryUserShellTest.cpp
        tests/coding_agent/runtime/SessionForkTest.cpp
        tests/coding_agent/runtime/SessionLifecycleTest.cpp
        tests/coding_agent/runtime/SessionSharedRuntimeTest.cpp
        tests/coding_agent/runtime/SessionSuiteGoldenTest.cpp
        tests/coding_agent/runtime/TreeNavigationTest.cpp
        tests/coding_agent/runtime/UserBashCancellationTest.cpp
        tests/coding_agent/runtime/UserBashOutputAccumulatorTest.cpp
        tests/coding_agent/runtime/UserBashOverlapTest.cpp
)
    target_include_directories(cch_tests_coding_agent PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_coding_agent
        PRIVATE
            cch_coding_agent_runtime
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_coding_agent PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_coding_agent PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_coding_agent ADD_TAGS_AS_LABELS)

    # coding-agent interactive TUI. Links the authoritative cch_cli owner too:
    # BootTrustInteractiveTest drives the in-process CLI seam (CliRunFixture).
    add_executable(cch_tests_coding_agent_interactive
        tests/Catch2Main.cpp
        tests/coding_agent/tui/BootTrustInteractiveTest.cpp
        tests/coding_agent/tui/ChatContainerStatusTest.cpp
        tests/coding_agent/tui/FooterStatusInteractiveTest.cpp
        tests/coding_agent/tui/FooterTest.cpp
        tests/coding_agent/tui/InteractiveBootE2ETest.cpp
        tests/coding_agent/tui/InteractiveModeTest.cpp
        tests/coding_agent/tui/InteractiveRenderingGoldenTest.cpp
        tests/coding_agent/tui/KeybindingsManagerTest.cpp
        tests/coding_agent/tui/LoadedResourcesTest.cpp
        tests/coding_agent/tui/LoginDialogTest.cpp
        tests/coding_agent/tui/LoginInteractiveModeTest.cpp
        tests/coding_agent/tui/ModelSelectorInteractiveTest.cpp
        tests/coding_agent/tui/ModelSelectorTest.cpp
        tests/coding_agent/tui/OAuthSelectorTest.cpp
        tests/coding_agent/tui/ProcessInteractiveModeTest.cpp
        tests/coding_agent/tui/ScopedModelsSelectorTest.cpp
        tests/coding_agent/tui/SessionSelectorComponentTest.cpp
        tests/coding_agent/tui/SessionSelectorInteractiveTest.cpp
        tests/coding_agent/tui/SessionSelectorSearchTest.cpp
        tests/coding_agent/tui/SessionReplacementTest.cpp
        tests/coding_agent/tui/SettingsSelectorTest.cpp
        tests/coding_agent/tui/StringListSelectorTest.cpp
        tests/coding_agent/tui/ThemeControllerTest.cpp
        tests/coding_agent/tui/ThemeTest.cpp
        tests/coding_agent/tui/TreeSelectorComponentTest.cpp
        tests/coding_agent/tui/TreeSelectorInteractiveTest.cpp
        tests/coding_agent/tui/TuiActionSeamTest.cpp
        tests/coding_agent/tui/UserBashInteractiveModeTest.cpp
)
    target_include_directories(cch_tests_coding_agent_interactive PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_coding_agent_interactive
        PRIVATE
            cch_cli
            cch_coding_agent_interactive
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_coding_agent_interactive PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_coding_agent_interactive PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_coding_agent_interactive ADD_TAGS_AS_LABELS)

    # CLI and architecture. The only shard that launches the built binary
    # (CliSmokeTest), so it carries the CCH_BINARY definition and a build
    # dependency on cpp_harness.
    add_executable(cch_tests_cli_arch
        tests/Catch2Main.cpp
        tests/cli/CliParseTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/cli/FrontendSelectionTest.cpp
        tests/cli/InitialPromptTest.cpp
        tests/cli/ListModelsTest.cpp
        tests/cli/PrintModeTest.cpp
        tests/cli/SessionFamilyCliTest.cpp
        tests/cli/StartupTuiTest.cpp
        tests/architecture/ArchitectureSurfaceScanTest.cpp
        tests/architecture/CMakeDependencyTest.cpp
        tests/architecture/MoveOnlyCallbackTest.cpp
        tests/architecture/PublicHeaderBoundaryTest.cpp
)
    target_include_directories(cch_tests_cli_arch PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_cli_arch
        PRIVATE
            cpp_harness_lib
            cch_cli
            CLI11::CLI11
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_cli_arch PRIVATE
        CCH_BINARY="$<TARGET_FILE:cpp_harness>"
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_cli_arch PRIVATE ${CCH_WARNING_OPTIONS})
    add_dependencies(cch_tests_cli_arch cpp_harness)
    catch_discover_tests(cch_tests_cli_arch ADD_TAGS_AS_LABELS)

    # Aggregate target that builds every shard (the Stage 4 entry point that
    # replaces the old single executable's build target).
    add_custom_target(cpp_harness_tests
        DEPENDS
            cch_tests_util
            cch_tests_support
            cch_tests_tui
            cch_tests_ai
            cch_tests_agent
            cch_tests_harness_tools
            cch_tests_coding_agent
            cch_tests_coding_agent_interactive
            cch_tests_cli_arch
)
endif()
