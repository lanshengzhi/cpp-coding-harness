include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

# Headless Session and Models composition. Frontend-specific sources live in
# frontend_tui and frontend_cli below, so this library can be built and used
# without compiling terminal interaction code (ADR 0053).
cch_parity_declare_target(
    TARGET cch_coding_agent
    ROLE owner
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/AgentConfigDir.cpp
        src/coding_agent/AgentSession.cpp
        src/coding_agent/AgentSessionCompaction.cpp
        src/coding_agent/AgentSessionExecution.cpp
        src/coding_agent/AgentSessionInteraction.cpp
        src/coding_agent/AuthStorage.cpp
        src/coding_agent/compat/pi/PiImport.cpp
        src/coding_agent/GitIgnoreMatcher.cpp
        src/coding_agent/ImageInput.cpp
        src/coding_agent/ModelConfig.cpp
        src/coding_agent/ModelResolver.cpp
        src/coding_agent/ModelRuntime.cpp
        src/coding_agent/ProjectResourceLoader.cpp
        src/coding_agent/ProjectResources.cpp
        src/coding_agent/ProjectTrust.cpp
        src/coding_agent/PromptTemplateLoader.cpp
        src/coding_agent/ProviderComposer.cpp
        src/coding_agent/RuntimeApiKeyOverlay.cpp
        src/coding_agent/SessionDiscovery.cpp
        src/coding_agent/SessionPathPolicy.cpp
        src/coding_agent/SettingsManager.cpp
        src/coding_agent/SkillFormatting.cpp
        src/coding_agent/SkillFrontmatterParser.cpp
        src/coding_agent/SkillLoader.cpp
        src/coding_agent/prompt/BuiltinSlashCommands.cpp
        src/coding_agent/prompt/PromptExpansion.cpp
        src/coding_agent/prompt/PromptTemplateExpander.cpp
        src/coding_agent/prompt/SystemPromptBuilder.cpp
        src/coding_agent/runtime/AuthGuidanceStream.cpp
        src/coding_agent/runtime/LocalUserShell.cpp
        src/coding_agent/runtime/SessionEventCommitment.cpp
        src/coding_agent/runtime/SessionFactory.cpp
        src/coding_agent/runtime/SessionFork.cpp
        src/coding_agent/runtime/SessionLifecycle.cpp
        src/coding_agent/runtime/SessionPersistence.cpp
        src/coding_agent/runtime/UserBashOutputAccumulator.cpp
    DEPENDS
        cch_agent_core
        cch_ai
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
        WebP::webpdecoder@webp
    INTERFACE_DEPENDS
        cch_agent_core
        cch_ai
        cch_support
)
cch_owner_include_roots(cch_coding_agent src/coding_agent/include)
target_include_directories(cch_coding_agent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)
# The System Prompt's identity-adjusted documentation block resolves the C++
# binary's own docs paths from the source tree.
target_compile_definitions(cch_coding_agent PRIVATE
    CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CCH_PROJECT_VERSION="${PROJECT_VERSION}"
)

# Native terminal presentation is an independently compiled frontend. It may
# depend on the headless application and the TUI toolkit, but the headless
# application never depends on this target.
cch_parity_declare_target(
    TARGET frontend_tui
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/tui/AssistantMessageComponent.cpp
        src/coding_agent/tui/AuthFlowController.cpp
        src/coding_agent/tui/BashExecutionComponent.cpp
        src/coding_agent/tui/ChatContainer.cpp
        src/coding_agent/tui/ClipboardPaste.cpp
        src/coding_agent/tui/ClipboardWrite.cpp
        src/coding_agent/tui/DiffRenderer.cpp
        src/coding_agent/tui/EditorAutocomplete.cpp
        src/coding_agent/tui/ExternalEditor.cpp
        src/coding_agent/tui/Footer.cpp
        src/coding_agent/tui/FooterDataProvider.cpp
        src/coding_agent/tui/InteractiveEngine.cpp
        src/coding_agent/tui/InteractiveEngineHost.cpp
        src/coding_agent/tui/InteractiveEngineWiring.cpp
        src/coding_agent/tui/InteractiveMode.cpp
        src/coding_agent/tui/InteractiveSessionRun.cpp
        src/coding_agent/tui/InteractiveStartup.cpp
        src/coding_agent/tui/InteractiveView.cpp
        src/coding_agent/tui/KeybindingHints.cpp
        src/coding_agent/tui/KeybindingsManager.cpp
        src/coding_agent/tui/LoadedResources.cpp
        src/coding_agent/tui/LoginDialog.cpp
        src/coding_agent/tui/LoginPresentation.cpp
        src/coding_agent/tui/ModelFlowController.cpp
        src/coding_agent/tui/ModelSelector.cpp
        src/coding_agent/tui/OAuthSelector.cpp
        src/coding_agent/tui/OpenBrowser.cpp
        src/coding_agent/tui/ReloadBox.cpp
        src/coding_agent/tui/ScopedModelsSelector.cpp
        src/coding_agent/tui/SessionFlowController.cpp
        src/coding_agent/tui/SessionFlowControllerTrust.cpp
        src/coding_agent/tui/SessionSelector.cpp
        src/coding_agent/tui/SessionSelectorSearch.cpp
        src/coding_agent/tui/SessionUiBinding.cpp
        src/coding_agent/tui/SettingsFlowController.cpp
        src/coding_agent/tui/SettingsSelector.cpp
        src/coding_agent/tui/SlashCommandEffects.cpp
        src/coding_agent/tui/SlashCommandRouter.cpp
        src/coding_agent/tui/StatusIndicator.cpp
        src/coding_agent/tui/SuspendController.cpp
        src/coding_agent/tui/Theme.cpp
        src/coding_agent/tui/ThemeController.cpp
        src/coding_agent/tui/ToolExecutionComponent.cpp
        src/coding_agent/tui/TreeSelector.cpp
        src/coding_agent/tui/UserMessageComponent.cpp
        src/coding_agent/tui/UserMessageSelector.cpp
    DEPENDS
        cch_coding_agent
        cch_agent_core
        cch_ai
        cch_tui
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
    INTERFACE_DEPENDS
        cch_coding_agent
        cch_agent_core
        cch_ai
        cch_tui
        cch_support
)
cch_owner_include_roots(frontend_tui src/coding_agent/include)
target_compile_definitions(frontend_tui PRIVATE
    CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CCH_PROJECT_VERSION="${PROJECT_VERSION}"
)

# The CLI adapter owns argument parsing, print mode, startup selection, and
# the bridge that chooses either the headless path or the native TUI. It links
# frontend_tui rather than making those presentation sources part of the
# headless Session library.
cch_parity_declare_target(
    TARGET frontend_cli
    ROLE implementation
    OWNER cch_coding_agent
    SOURCES
        src/cli/AsyncCliRuntime.cpp
        src/cli/CliParse.cpp
        src/cli/FrontendSelection.cpp
        src/cli/InitialPrompt.cpp
        src/cli/ListModels.cpp
        src/cli/PrintMode.cpp
        src/cli/SessionFamily.cpp
        src/cli/StartupTui.cpp
    DEPENDS
        cch_coding_agent
        frontend_tui
        cch_agent_core
        cch_ai
        cch_tui
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
    INTERFACE_DEPENDS
        cch_coding_agent
        cch_agent_core
        cch_ai
        cch_tui
        cch_support
)
cch_owner_include_roots(frontend_cli src/coding_agent/include)
target_compile_definitions(frontend_cli PRIVATE
    CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CCH_PROJECT_VERSION="${PROJECT_VERSION}"
)
