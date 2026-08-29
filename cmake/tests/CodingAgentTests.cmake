include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

    # harness and tools
    add_executable(cch_tests_harness_tools
        tests/Catch2Main.cpp
        tests/harness/AsyncLocalExecutionEnvTest.cpp
        tests/harness/OutputLimiterTest.cpp
        tests/harness/ProcessTest.cpp
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
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_harness_tools PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_harness_tools PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_harness_tools ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_harness_tools ${CCH_PARITY_BUILD_GATE_TARGET})

    # coding-agent composition (Session, Runtime, Models Runtime)
    add_executable(cch_tests_coding_agent
        tests/Catch2Main.cpp
        tests/support/ScriptedProvider.cpp
        tests/support/ModelRuntimeTestSupport.cpp
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
        tests/coding_agent/runtime/SessionAssemblyContractTest.cpp
        tests/coding_agent/runtime/SessionForkTest.cpp
        tests/coding_agent/runtime/SessionLifecycleTest.cpp
        tests/coding_agent/runtime/SessionReplacementFactsTest.cpp
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
            cch_coding_agent
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_coding_agent PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_coding_agent PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_coding_agent ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_coding_agent ${CCH_PARITY_BUILD_GATE_TARGET})

    # coding-agent interactive TUI composition. BootTrustInteractiveTest
    # drives the in-process CLI seam (CliRunFixture) through the one
    # repository-private cch_coding_agent library (#468).
    add_executable(cch_tests_coding_agent_interactive
        tests/Catch2Main.cpp
        tests/support/ScriptedProvider.cpp
        tests/support/ModelRuntimeTestSupport.cpp
        tests/coding_agent/tui/BootTrustInteractiveTest.cpp
        tests/coding_agent/tui/ChatContainerStatusTest.cpp
        tests/coding_agent/tui/FooterStatusInteractiveTest.cpp
        tests/coding_agent/tui/FooterTest.cpp
        tests/coding_agent/tui/InteractiveBootE2ETest.cpp
        tests/coding_agent/tui/InteractiveModeTest.cpp
        tests/coding_agent/tui/InteractiveRenderingGoldenTest.cpp
        tests/coding_agent/tui/InteractiveSessionRunTest.cpp
        tests/coding_agent/tui/InteractiveViewActionTest.cpp
        tests/coding_agent/tui/KeybindingsManagerTest.cpp
        tests/coding_agent/tui/LoadedResourcesTest.cpp
        tests/coding_agent/tui/LoginDialogTest.cpp
        tests/coding_agent/tui/LoginInteractiveModeTest.cpp
        tests/coding_agent/tui/ModelFlowControllerTest.cpp
        tests/coding_agent/tui/ModelSelectorInteractiveTest.cpp
        tests/coding_agent/tui/ModelSelectorTest.cpp
        tests/coding_agent/tui/OAuthSelectorTest.cpp
        tests/coding_agent/tui/ProcessInteractiveModeTest.cpp
        tests/coding_agent/tui/ScopedModelsSelectorTest.cpp
        tests/coding_agent/tui/SlashCommandRouterTest.cpp
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
            cch_coding_agent
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_coding_agent_interactive PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_coding_agent_interactive PRIVATE ${CCH_WARNING_OPTIONS})
    # The four InteractiveBootE2ETest cases share the fixed byte-stable
    # workspace directory the CLI-level goldens render (#529): discover them
    # separately so they carry a RESOURCE_LOCK that serializes the sibling
    # processes under the parallel ctest default, while everything else
    # discovers without the lock. The two discovery passes partition the
    # executable's cases by the cases' unique `E2E: ` name prefix.
    catch_discover_tests(cch_tests_coding_agent_interactive ADD_TAGS_AS_LABELS
        TEST_SPEC "~E2E: *")
    catch_discover_tests(cch_tests_coding_agent_interactive ADD_TAGS_AS_LABELS
        TEST_SPEC "E2E: *"
        PROPERTIES RESOURCE_LOCK "cch-interactive-boot-e2e-workspace")
    add_dependencies(cch_tests_coding_agent_interactive ${CCH_PARITY_BUILD_GATE_TARGET})
