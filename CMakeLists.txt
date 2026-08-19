cmake_minimum_required(VERSION 4.4)

set(CCH_TOOLCHAIN_ROLE "build" CACHE STRING "Supported compiler role: build or conformance")
set_property(CACHE CCH_TOOLCHAIN_ROLE PROPERTY STRINGS build conformance)

include(cmake/SupportedBuild.cmake)
cch_validate_supported_build_before_project()

project(cpp_coding_harness VERSION 0.1.0 LANGUAGES CXX)
cch_validate_supported_build_after_project()

# Release-artifact IPO/LTO (ADR 0039; issue #474): the vcpkg-release-artifact
# preset opts in through CMAKE_INTERPROCEDURAL_OPTIMIZATION. Validate IPO at
# configure time so an unsupported toolchain fails closed instead of silently
# producing a non-LTO "release artifact".
if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT cch_ipo_supported OUTPUT cch_ipo_error LANGUAGES CXX)
    if(NOT cch_ipo_supported)
        message(FATAL_ERROR
            "IPO/LTO was requested but this toolchain does not support it: ${cch_ipo_error}")
    endif()
    message(STATUS "IPO/LTO enabled and validated for this configuration")
endif()

# CMake 3.30+ removed the legacy FindBoost module; use Boost's own config-mode package.
cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CCH_BUILD_TESTS "Build tests" ON)
option(CCH_STRICT_NO_EXCEPTIONS
    "Require -fno-exceptions and the strict exception allowlist"
    ON)
if(CCH_STRICT_NO_EXCEPTIONS)
    # The strict no-exception policy is the default build (ADR 0042): ordinary
    # failure is cch::support::Expected / std::error_code, and an exception is
    # not a second error channel. It applies to every project-owned C++ target,
    # including test shards. Disabling the option is a deliberate deviation
    # from the supported policy and is intended only for local debugging.
    add_compile_options(-fno-exceptions)
    # Boost.Asio's completion machinery must take the same no-exception path
    # as the project-owned code. The compiler flag also makes Boost.Config set
    # BOOST_NO_EXCEPTIONS, but this explicit definition keeps the Asio policy
    # visible in every strict compile command.
    add_compile_definitions(BOOST_ASIO_NO_EXCEPTIONS)
    message(STATUS "Strict no-exception policy enabled")
else()
    message(WARNING
        "CCH_STRICT_NO_EXCEPTIONS is OFF: the default strict no-exception "
        "policy (ADR 0042) is disabled for this configuration")
endif()

find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)

cch_require_vcpkg_dependency("Boost" "${Boost_DIR}")
cch_require_vcpkg_dependency("OpenSSL headers" "${OPENSSL_INCLUDE_DIR}")
cch_require_vcpkg_dependency("OpenSSL SSL library" "${OPENSSL_SSL_LIBRARY}")
cch_require_vcpkg_dependency("OpenSSL crypto library" "${OPENSSL_CRYPTO_LIBRARY}")
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

# Sanitizer coverage (issue #473). The blocking sanitizer CI jobs configure
# through the checked-in vcpkg-asan-ubsan / vcpkg-tsan presets, which set this
# cache variable; validation is fail-closed so an unknown selection is a
# configure error, never a silently unsanitized build. Flags apply to every
# target declared below, production and test alike.
set(CCH_SANITIZER "" CACHE STRING "Sanitizer selection: empty, 'address;undefined', or 'thread'")
cch_validate_sanitizer_selection("${CCH_SANITIZER}")
if(CCH_SANITIZER)
    string(REPLACE ";" "," sanitizer_commas "${CCH_SANITIZER}")
    add_compile_options("-fsanitize=${sanitizer_commas}" -fno-omit-frame-pointer)
    add_link_options("-fsanitize=${sanitizer_commas}")
    message(STATUS "Sanitizer coverage: ${sanitizer_commas}")
endif()

# Central Parity Architecture constructors (ADR 0039). Every production target
# is declared through cch_parity_declare_target so its role, Owner, source
# ownership, direct dependencies, interface visibility, and external-family
# classification are recorded as configured evidence and emitted as the
# resolved ownership index. The manifest is the strict policy authority the
# Gate validates that evidence against.
include(cmake/ParityArchitecture.cmake)
cch_parity_set_manifest("${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json")

# Owner-local include roots (ADR 0039; #469). Each package publishes only its
# own interface root (<package>/include, which provides the canonical
# <cch/...> spelling) to dependents and keeps the repository-private src root
# private. There is no global public header root: deleting a package's
# interface root removes its headers from every dependent's include path.
function(cch_owner_include_roots target_name interface_root)
    target_include_directories(${target_name}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/${interface_root}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endfunction()

# Pi-neutral C++ Support Package (ADR 0039; #469). Owns passive Error/Expected,
# JsonValue (including its hand-rolled I/O), AsyncResult, and shared move-only
# mechanics used across Owners. It carries no product policy, process policy,
# serialization DTOs, redaction, or output-limiting policy; those remain with
# their Capability Owners.
cch_parity_declare_target(
    TARGET cch_support
    ROLE support
    OWNER cch_support
    SOURCES
        src/support/Json.cpp
)
cch_owner_include_roots(cch_support src/support/include)

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
        src/tui/OverlayCompositor.cpp
        src/tui/ProcessTerminal.cpp
        src/tui/SelectList.cpp
        src/tui/SettingsList.cpp
        src/tui/TerminalImage.cpp
        src/tui/Text.cpp
        src/tui/TextBuffer.cpp
        src/tui/TruncatedText.cpp
        src/tui/Tui.cpp
        src/tui/UnicodeWidth.cpp
        src/tui/Utils.cpp
        src/tui/VirtualTerminal.cpp
    DEPENDS
        cch_support
        md4c::md4c@md4c
        utf8proc::utf8proc@utf8proc
        Threads::Threads@threads
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_tui src/tui/include)

cch_parity_declare_target(
    TARGET cch_ai
    ROLE owner
    OWNER cch_ai
    SOURCES
        src/ai/BoostExceptionHandler.cpp
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
        src/ai/providers/ComposedProvider.cpp
        src/ai/providers/EnvApiKeyAuth.cpp
        src/ai/providers/FakeProvider.cpp
        src/ai/providers/KimiCatalog.cpp
        src/ai/providers/RetryPolicy.cpp
        src/ai/providers/SseParser.cpp
        src/ai/providers/StreamExecutionEngine.cpp
        src/ai/utils/RetryClassifier.cpp
    DEPENDS
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
        OpenSSL::SSL@openssl
        OpenSSL::Crypto@openssl
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_ai src/ai/include)
# Boost's no-exception configuration leaves the application-owned
# boost::throw_exception termination hook to the consuming target. Compile the
# private hook without exceptions even if the strict policy is disabled locally
# (ADR 0042).
set_source_files_properties(src/ai/BoostExceptionHandler.cpp PROPERTIES
    COMPILE_OPTIONS -fno-exceptions
    COMPILE_DEFINITIONS BOOST_ASIO_NO_EXCEPTIONS
)

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
        src/harness/Process.cpp
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
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
        utf8proc::utf8proc@utf8proc
    INTERFACE_DEPENDS
        cch_ai
        cch_support
)
cch_owner_include_roots(cch_agent_core src/agent/include)

# Repository-private coding-agent Owner Package (ADR 0039; #468). One
# authoritative compiled static library owns every coding-agent composition
# production source exactly once: Models Runtime (Model/Models composition,
# Agent Config Directory, auth storage), Session, Runtime, Native TUI
# composition, and CLI composition. Its only cross-Owner dependencies are the
# legal edges to cch_agent_core, cch_ai, and cch_tui, plus the pi-neutral
# support package; no other Owner depends on it. The stateful Agent consumes
# the AI-owned move-only ModelStream (ADR 0040 / #453), so no reverse
# cch_agent_core -> cch_coding_agent edge exists (#456/#460).
cch_parity_declare_target(
    TARGET cch_coding_agent
    ROLE owner
    OWNER cch_coding_agent
    SOURCES
        src/coding_agent/AgentConfigDir.cpp
        src/coding_agent/AgentSession.cpp
        src/coding_agent/AuthStorage.cpp
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
        src/coding_agent/runtime/AgentSessionRuntime.cpp
        src/coding_agent/runtime/AsyncCliRuntime.cpp
        src/coding_agent/runtime/AuthGuidanceStream.cpp
        src/coding_agent/runtime/LocalUserShell.cpp
        src/coding_agent/runtime/SessionEventCommitment.cpp
        src/coding_agent/runtime/SessionFactory.cpp
        src/coding_agent/runtime/SessionFork.cpp
        src/coding_agent/runtime/SessionLifecycle.cpp
        src/coding_agent/runtime/SessionPersistence.cpp
        src/coding_agent/runtime/UserBashOutputAccumulator.cpp
        src/coding_agent/tui/AssistantMessageComponent.cpp
        src/coding_agent/tui/BashExecutionComponent.cpp
        src/coding_agent/tui/ChatContainer.cpp
        src/coding_agent/tui/ClipboardWrite.cpp
        src/coding_agent/tui/DiffRenderer.cpp
        src/coding_agent/tui/ExternalEditor.cpp
        src/coding_agent/tui/Footer.cpp
        src/coding_agent/tui/FooterDataProvider.cpp
        src/coding_agent/tui/InteractiveMode.cpp
        src/coding_agent/tui/InteractiveView.cpp
        src/coding_agent/tui/KeybindingHints.cpp
        src/coding_agent/tui/KeybindingsManager.cpp
        src/coding_agent/tui/LoadedResources.cpp
        src/coding_agent/tui/LoginDialog.cpp
        src/coding_agent/tui/LoginPresentation.cpp
        src/coding_agent/tui/ModelSelector.cpp
        src/coding_agent/tui/OAuthSelector.cpp
        src/coding_agent/tui/OpenBrowser.cpp
        src/coding_agent/tui/ReloadBox.cpp
        src/coding_agent/tui/ScopedModelsSelector.cpp
        src/coding_agent/tui/SessionSelector.cpp
        src/coding_agent/tui/SessionSelectorSearch.cpp
        src/coding_agent/tui/SettingsSelector.cpp
        src/coding_agent/tui/StatusIndicator.cpp
        src/coding_agent/tui/StringListSelector.cpp
        src/coding_agent/tui/Theme.cpp
        src/coding_agent/tui/ThemeController.cpp
        src/coding_agent/tui/ToolExecutionComponent.cpp
        src/coding_agent/tui/TreeSelector.cpp
        src/coding_agent/tui/UserMessageComponent.cpp
        src/coding_agent/tui/UserMessageSelector.cpp
        src/cli/CliParse.cpp
        src/cli/FrontendSelection.cpp
        src/cli/InitialPrompt.cpp
        src/cli/ListModels.cpp
        src/cli/PrintMode.cpp
        src/cli/SessionFamily.cpp
        src/cli/StartupTui.cpp
    DEPENDS
        cch_agent_core
        cch_ai
        cch_tui
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
# binary's own docs paths from the source tree (pi `config.ts`
# getReadmePath/getDocsPath/getExamplesPath resolve the pi package).
target_compile_definitions(cch_coding_agent PRIVATE
    CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    CCH_PROJECT_VERSION="${PROJECT_VERSION}"
)

# The executable is a thin closure over the repository-private
# cch_coding_agent library (#468): it compiles only the entry point and links
# the one authoritative Owner library.
cch_parity_declare_target(
    TARGET cpp_harness
    ROLE composition
    OWNER cch_coding_agent
    KIND executable
    SOURCES
        src/main.cpp
    DEPENDS
        cch_coding_agent
)
# main.cpp compiles against the Owner's private runtime headers (the src
# root); those headers reference the cch_tui interface, which is a legal
# private cch_coding_agent edge and therefore not interface-published, so the
# composition target adds the tui interface root privately.
target_include_directories(cpp_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tui/include
)

# Mandatory fail-closed Parity Architecture Gate at configure time (ADR 0039;
# issue #470). Configure validates the resolved ownership index and the
# direct-include evidence against the strict manifest for every supported
# configuration, including configurations with tests disabled; a rejection
# fails configuration with the validator's deterministic report. The
# build-phase Gate (compile commands and active depfile evidence) is wired in
# after all production targets are declared (cch_parity_add_build_gate below).
set(CCH_PARITY_EXTERNAL_INCLUDE_ROOTS
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
cch_parity_run_gate(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    EXTERNAL_INCLUDE_ROOTS ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
    STRICT_NO_EXCEPTIONS ${CCH_STRICT_NO_EXCEPTIONS}
)

# Mandatory build-phase Parity Architecture Gate (ADR 0039; issue #470). The
# `cch_parity_gate_build` target is a member of `all`, so every normal build
# runs or depends on the latest applicable Gate phase: it depends on every
# declared production target, so its command runs after all production
# compilation has produced fresh depfiles, then records the active dependency
# evidence and validates manifest, index, direct-include, compile-command, and
# depfile evidence, failing the build on any rejection. The composition
# executable additionally carries the same Gate as a POST_BUILD step, so a
# direct production-target build (`--target cpp_harness`) also requires fresh
# successful Gate evidence. Test executables depend on
# ${CCH_PARITY_BUILD_GATE_TARGET} so every CTest entry point requires it.
cch_parity_add_build_gate(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    BUILD_DIR "${CMAKE_BINARY_DIR}"
    EXTERNAL_INCLUDE_ROOTS ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
    STRICT_NO_EXCEPTIONS ${CCH_STRICT_NO_EXCEPTIONS}
)
cch_parity_attach_build_gate_post_build(cpp_harness)

# Runtime-only install surface (ADR 0039; issue #472). `cmake --install`
# installs only the cpp_harness Runtime under bin/ and the required
# third-party license/notice texts under share/cpp_harness/licenses/ — no
# Owner Interface headers, static libraries, CMake package metadata, exported
# targets, or other development surface. The install path runs the fail-closed
# install gate before any file rule: the build-phase Parity Architecture Gate,
# the Gate-evidence freshness check, and the dependency-closure audit must all
# pass, so installation cannot bypass architecture validation.
include(cmake/RuntimeInstall.cmake)
cch_runtime_collect_licenses(CCH_RUNTIME_LICENSE_FILES)
cch_runtime_install_rules(
    TARGET cpp_harness
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json"
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    LICENSE_FILES ${CCH_RUNTIME_LICENSE_FILES}
    EXTERNAL_INCLUDE_ROOTS ${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
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
    cch_add_supported_build_policy_test("accept-sanitizer-address-undefined" TRUE "")
    cch_add_supported_build_policy_test("accept-sanitizer-thread" TRUE "")
    cch_add_supported_build_policy_test("reject-unknown-sanitizer" FALSE "Unknown CCH_SANITIZER entry")
    cch_add_supported_build_policy_test("reject-sanitizer-combination" FALSE "cannot be combined")
    cch_add_supported_build_policy_test("reject-duplicate-sanitizer" FALSE "Duplicate CCH_SANITIZER entry")
    cch_add_supported_build_policy_test("reject-dependency-fallback" FALSE "outside the pinned vcpkg prefix")
    cch_add_supported_build_policy_test("reject-missing-vcpkg-toolchain" FALSE "pinned vcpkg toolchain")
    cch_add_supported_build_policy_test("reject-vcpkg-manifest-install" FALSE "manifest installation is required")
    cch_add_supported_build_policy_test("reject-vcpkg-host-triplet" FALSE "host triplet")
    cch_add_supported_build_policy_test("reject-vcpkg-overlays" FALSE "overlay ports")

    # Parity Architecture Gate (ADR 0039). The Python validator uses only the
    # standard library and Python 3.12+; it is a system tool, not a vcpkg
    # dependency. The unit case exercises the strict manifest, the
    # direct-include lexer and canonical resolution, compile-context,
    # depfile-evidence, and evidence-producer rules directly; the fixture case
    # configures a minimal legal fixture, an illegal cross-Owner target-edge
    # fixture, and a set of poison-source fixtures end to end and asserts the
    # stable rule-ID rejections (PARITY-2001, PARITY-4xxx, PARITY-6001).
    find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME cch_parity_gate_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/parity_gate_test.py
    )
    set_tests_properties(cch_parity_gate_unit PROPERTIES
        LABELS "architecture;parity-gate;issue448;issue449;issue470;issue480")

    add_test(
        NAME cch_parity_gate_fixture
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/ParityGateTest.cmake
    )
    set_tests_properties(cch_parity_gate_fixture PROPERTIES
        LABELS "architecture;parity-gate;issue448;issue449;issue470;issue480")

    # Runtime-only install (ADR 0039; issue #472). The unit case covers the
    # dependency-closure audit and the Gate-evidence freshness check with
    # synthetic fixtures; the fixture case drives a minimal project through
    # the real install rules to prove the install path fails closed without
    # fresh successful Gate evidence, stages only bin/ + licenses, and
    # rejects a stale (edited-but-not-rebuilt) tree.
    add_test(
        NAME cch_install_tools_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/install_tools_test.py
    )
    set_tests_properties(cch_install_tools_unit PROPERTIES LABELS "install;issue472")

    add_test(
        NAME cch_install_gate_fixture
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            "-DCCH_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/InstallGateTest.cmake
    )
    set_tests_properties(cch_install_gate_fixture PROPERTIES LABELS "install;issue472")

    # Release qualification evidence (ADR 0039; issue #474). The fail-closed
    # verifier checks the artifact lane's evidence directory: presence,
    # closed vocabularies, freshness against the run window, toolchain
    # contradiction rules, required PASS markers, and the artifact digest
    # binding; missing, stale, contradictory, or incomplete evidence fails
    # qualification rather than degrading to a warning.
    add_test(
        NAME cch_release_evidence_unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/release_evidence_test.py
    )
    set_tests_properties(cch_release_evidence_unit PROPERTIES LABELS "install;release;issue474")

    # Production build-phase Gate self-check (ADR 0039; issue #470): run the
    # same fail-closed build-phase Gate against the production evidence
    # (manifest, index, direct-include evidence, compile commands, and fresh
    # depfile evidence) and assert it passes. This proves the production
    # repository satisfies every manifest rule after package contraction and
    # that the deterministic machine-readable report is written. It runs after
    # the build has produced fresh depfiles; the build-phase Gate custom
    # target is already a prerequisite of every test shard.
    add_test(
        NAME cch_parity_gate_production_build
        COMMAND
            ${CMAKE_COMMAND}
            -DCCH_PARITY_GATE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/parity_gate.py
            -DCCH_PARITY_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json
            -DCCH_PARITY_INDEX=${CMAKE_BINARY_DIR}/parity-ownership-index.json
            -DCCH_PARITY_DIRECT_INCLUDES=${CMAKE_BINARY_DIR}/parity-direct-includes.json
            -DCCH_PARITY_COMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
            -DCCH_PARITY_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DCCH_PARITY_DEPFILES=${CMAKE_BINARY_DIR}/parity-build-gate-depfiles.json
            -DCCH_PARITY_REPORT=${CMAKE_BINARY_DIR}/parity-build-gate.json
            -DCCH_PARITY_EXTERNAL_INCLUDE_ROOTS=${CCH_PARITY_EXTERNAL_INCLUDE_ROOTS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/run-build-gate.cmake
    )
    set_tests_properties(cch_parity_gate_production_build PROPERTIES
        LABELS "architecture;parity-gate;issue470;issue480")

    # Owner Interface standalone compile (ADR 0039; #469): every Owner
    # Interface header compiles alone with the include path restricted to its
    # declared package interface dependencies (own root, support, legal direct
    # Owner dependencies). No third-party or private root is provided, so
    # third-party leakage, private paths, and undeclared Owner edges fail.
    add_test(
        NAME cch_owner_interface_standalone
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/owner_interface_standalone.py
            --compiler ${CMAKE_CXX_COMPILER}
            --manifest ${CMAKE_CURRENT_SOURCE_DIR}/cmake/parity/manifest.json
            --project-root ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(cch_owner_interface_standalone PROPERTIES LABELS "architecture;parity-gate;issue469")

    # Package-aligned test shard executables (issue #447; CODING_STANDARDS.md
    # section 11.6). Shards are build grouping only: `catch_discover_tests`
    # registers Catch2 cases as individual CTest tests, and CTest names/labels
    # are the sole selection, scheduling, timeout, and reporting authority.
    # Each shard is its own executable, so a focused test edit builds and links
    # only its owning package. `cpp_harness_tests` remains as the aggregate
    # target that builds every shard. Grouping follows the Capability Owner or
    # support ownership in CODING_STANDARDS.md section 11.5 and fixture
    # ownership, not equal file counts; no test object is compiled twice.
    # Tests include Owner Interface headers through the owner-local interface
    # roots and private headers through the repository-private src root.
    set(CCH_FORMAL_TEST_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ai/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/agent/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tui/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/coding_agent/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/support/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

    # support
    add_executable(cch_tests_support
        tests/Catch2Main.cpp
        tests/support/AllocationCounter.cpp
        tests/support/AsyncResultTest.cpp
        tests/support/ExpectedErrorTest.cpp
        tests/support/ExpectedMacrosTest.cpp
        tests/support/JsonValueIoTest.cpp
        tests/support/TestRunnerIsolationTest.cpp
)
    target_include_directories(cch_tests_support PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_support
        PRIVATE
            cch_support
            glaze::glaze
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
    # Every test entry point requires fresh successful build-phase Gate evidence
    # (ADR 0039; #470): the shard builds the Gate after all production targets.
    add_dependencies(cch_tests_support ${CCH_PARITY_BUILD_GATE_TARGET})

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
        tests/tui/OverlayCompositorTest.cpp
        tests/tui/PiTuiDifferentialTest.cpp
        tests/tui/ProcessTerminalTest.cpp
        tests/tui/RenderDifferentialTest.cpp
        tests/tui/ScreenStateGoldenTest.cpp
        tests/tui/SelectListTest.cpp
        tests/tui/SettingsListTest.cpp
        tests/tui/TerminalImageTest.cpp
        tests/tui/TextBufferTest.cpp
        tests/tui/TruncatedTextTest.cpp
        tests/tui/TuiTest.cpp
        tests/tui/UnicodeWidthTest.cpp
        tests/tui/UtilsTest.cpp
        tests/tui/VirtualTerminalTest.cpp
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
    add_dependencies(cch_tests_tui ${CCH_PARITY_BUILD_GATE_TARGET})

    # AI
    add_executable(cch_tests_ai
        tests/Catch2Main.cpp
        tests/ai/BoundedTextTest.cpp
        tests/ai/GlazeRoundTripTest.cpp
        tests/ai/MessageContractTest.cpp
        tests/ai/MessageConversionTest.cpp
        tests/ai/ModelTest.cpp
        tests/ai/ModelsTest.cpp
        tests/ai/ProviderPolicyTest.cpp
        tests/ai/RetryClassifierTest.cpp
        tests/ai/RedactorTest.cpp
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
        tests/ai/providers/StreamExecutionEngineTest.cpp
)
    target_include_directories(cch_tests_ai PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_ai
        PRIVATE
            cch_agent_core
            cch_ai
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_ai PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_ai PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_ai ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_ai ${CCH_PARITY_BUILD_GATE_TARGET})

    # Private Boost.Asio bridges carry the only exception-shaped detail in the
    # strict policy (ADR 0042): this no-exception test target validates the
    # bridge contract directly.
    add_executable(cch_tests_ai_async_bridge
        tests/Catch2Main.cpp
        tests/ai/AsyncBridgeTest.cpp
    )
    target_include_directories(cch_tests_ai_async_bridge PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_ai_async_bridge
        PRIVATE
            cch_ai
            Boost::headers
            Catch2::Catch2
    )
    target_compile_definitions(cch_tests_ai_async_bridge PRIVATE
        BOOST_ASIO_NO_EXCEPTIONS
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(cch_tests_ai_async_bridge PRIVATE
        ${CCH_WARNING_OPTIONS}
        -fno-exceptions
    )
    catch_discover_tests(cch_tests_ai_async_bridge ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_ai_async_bridge ${CCH_PARITY_BUILD_GATE_TARGET})

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
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_agent PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_agent PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_agent ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_agent ${CCH_PARITY_BUILD_GATE_TARGET})

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
        tests/coding_agent/tui/BootTrustInteractiveTest.cpp
        tests/coding_agent/tui/ChatContainerStatusTest.cpp
        tests/coding_agent/tui/FooterStatusInteractiveTest.cpp
        tests/coding_agent/tui/FooterTest.cpp
        tests/coding_agent/tui/InteractiveBootE2ETest.cpp
        tests/coding_agent/tui/InteractiveModeTest.cpp
        tests/coding_agent/tui/InteractiveRenderingGoldenTest.cpp
        tests/coding_agent/tui/InteractiveViewActionTest.cpp
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
            cch_coding_agent
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_coding_agent_interactive PRIVATE
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
    target_compile_options(cch_tests_coding_agent_interactive PRIVATE ${CCH_WARNING_OPTIONS})
    catch_discover_tests(cch_tests_coding_agent_interactive ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_coding_agent_interactive ${CCH_PARITY_BUILD_GATE_TARGET})

    # CLI and architecture. The only shard that launches the built binary
    # (CliSmokeTest), so it carries the CCH_BINARY definition and a build
    # dependency on cpp_harness.
    add_executable(cch_tests_cli_arch
        tests/Catch2Main.cpp
        tests/cli/CliParseTest.cpp
        tests/cli/CliSmokeTest.cpp
        tests/cli/FrontendSelectionTest.cpp
        tests/cli/InitialPromptTest.cpp
        tests/cli/InstallRelocationTest.cpp
        tests/cli/ListModelsTest.cpp
        tests/cli/PrintModeTest.cpp
        tests/cli/SessionFamilyCliTest.cpp
        tests/cli/StartupTuiTest.cpp
        tests/architecture/MoveOnlyCallbackTest.cpp
        tests/architecture/PublicHeaderBoundaryTest.cpp
)
    target_include_directories(cch_tests_cli_arch PRIVATE ${CCH_FORMAL_TEST_INCLUDE_DIRS})
    target_link_libraries(cch_tests_cli_arch
        PRIVATE
            cch_coding_agent
            Boost::headers
            Catch2::Catch2
)
    target_compile_definitions(cch_tests_cli_arch PRIVATE
        CCH_BINARY="$<TARGET_FILE:cpp_harness>"
        CCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        CCH_BUILD_DIR="${CMAKE_BINARY_DIR}"
        CCH_CMAKE_COMMAND="${CMAKE_COMMAND}"
        CCH_PYTHON3="${Python3_EXECUTABLE}"
)
    target_compile_options(cch_tests_cli_arch PRIVATE ${CCH_WARNING_OPTIONS})
    add_dependencies(cch_tests_cli_arch cpp_harness)
    # Sanitizer builds link the sanitizer runtimes and are not installable:
    # the relocation test skips itself under them (issue #473).
    if(NOT CCH_SANITIZER STREQUAL "")
        target_compile_definitions(cch_tests_cli_arch PRIVATE CCH_SANITIZER_BUILD=1)
    endif()
    catch_discover_tests(cch_tests_cli_arch ADD_TAGS_AS_LABELS)
    add_dependencies(cch_tests_cli_arch ${CCH_PARITY_BUILD_GATE_TARGET})

    # Aggregate target that builds every shard; a build convenience, not a
    # scheduling authority (issue #447; CODING_STANDARDS.md section 11.6).
    add_custom_target(cpp_harness_tests
        DEPENDS
            cch_tests_support
            cch_tests_tui
            cch_tests_ai
            cch_tests_ai_async_bridge
            cch_tests_agent
            cch_tests_harness_tools
            cch_tests_coding_agent
            cch_tests_coding_agent_interactive
            cch_tests_cli_arch
            ${CCH_PARITY_BUILD_GATE_TARGET}
)
endif()
