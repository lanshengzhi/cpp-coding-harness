#include "support/TextHelpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CCH_SOURCE_DIR
#define CCH_SOURCE_DIR "."
#endif

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string cmake_command_block(const std::string& cmake, const std::string& command_start) {
    const auto start = cmake.find(command_start);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = cmake.find("\n)", start);
    if (end == std::string::npos) {
        return cmake.substr(start);
    }
    return cmake.substr(start, end - start + 2);
}

/// The central `cch_parity_declare_target(...)` block for `target` (the
/// `TARGET <name>` line through the block's closing `)`). The trailing newline
/// disambiguates prefix-sharing names such as `cpp_harness`/`cpp_harness_lib`.
std::string target_decl_block(const std::string& cmake, const std::string& target) {
    return cmake_command_block(cmake, "TARGET " + target + "\n");
}

/// The dependency list inside a central declaration: from `DEPENDS` through
/// the block's closing `)`, so it covers `INTERFACE_DEPENDS` too. Checking this
/// section instead of the whole block keeps the OWNER/ROLE/TARGET lines from
/// tripping substring assertions (e.g. `cch_agent` inside `cch_coding_agent`).
std::string depends_section(const std::string& cmake, const std::string& target) {
    const auto block = target_decl_block(cmake, target);
    const auto start = block.find("DEPENDS");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = block.find("\n)", start);
    if (end == std::string::npos) {
        return block.substr(start);
    }
    return block.substr(start, end - start);
}

bool block_mentions(const std::string& block, const std::string& token) {
    return block.find(token) != std::string::npos;
}

std::vector<std::filesystem::path> files_under(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(root)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// The unique top-level test modules (`tests/<module>/`) named in `block`.
std::vector<std::string> test_modules_in(const std::string& block) {
    std::vector<std::string> modules;
    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find("tests/");
        if (pos == std::string::npos) {
            continue;
        }
        const auto slash = line.find('/', pos + 6);
        if (slash != std::string::npos) {
            modules.push_back(line.substr(pos, slash - pos));
        }
    }
    std::sort(modules.begin(), modules.end());
    modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
    return modules;
}

/// The immediate children named after `tests/coding_agent/` in `block`:
/// root-level stems (`AgentConfigDirTest.cpp`) or subdirectories (`runtime`,
/// `tui`). Used to split the coding-agent package into core/runtime and
/// interactive shards that share the top-level module.
std::vector<std::string> coding_agent_children(const std::string& block) {
    std::vector<std::string> children;
    const std::string marker = "tests/coding_agent/";
    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        const auto end = line.find('/', pos + marker.size());
        const auto child = line.substr(pos + marker.size(), end - pos - marker.size());
        if (!child.empty()) {
            children.push_back(child);
        }
    }
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()), children.end());
    return children;
}

} // namespace

TEST_CASE("CMake declares pi package-style targets", "[architecture][cmake][issue56][issue57][issue58]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    CHECK(block_mentions(cmake, "TARGET cch_util\n"));
    CHECK(block_mentions(cmake, "TARGET cch_tui\n"));
    const auto tui_sources = target_decl_block(cmake, "cch_tui");
    CHECK(block_mentions(tui_sources, "src/tui/Autocomplete.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/CancellableLoader.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Image.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Input.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/InputDecoder.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Keys.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Keybindings.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Loader.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Markdown.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/SelectList.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/SettingsList.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/TerminalImage.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Text.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Tui.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/VirtualTerminal.cpp"));
    CHECK(block_mentions(cmake, "TARGET cch_ai\n"));
    CHECK(block_mentions(cmake, "TARGET cch_agent_core\n"));
    const auto agent_core_sources = target_decl_block(cmake, "cch_agent_core");
    CHECK(block_mentions(agent_core_sources, "src/agent/Agent.cpp"));
    CHECK(block_mentions(agent_core_sources, "src/agent/AgentLoop.cpp"));
    CHECK(block_mentions(agent_core_sources, "src/harness/ShellResolver.cpp"));
    CHECK(block_mentions(agent_core_sources, "src/tools/AsyncToolFactories.cpp"));
    // One repository-private cch_coding_agent library owns every coding-agent
    // composition source (#468): Models Runtime, Session, Runtime, Native TUI
    // composition, and CLI composition.
    CHECK(block_mentions(cmake, "TARGET cch_coding_agent\n"));
    const auto coding_agent_sources = target_decl_block(cmake, "cch_coding_agent");
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/ModelRuntime.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/ModelConfig.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/ProviderComposer.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/AgentSession.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/runtime/AgentSessionRuntime.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/tui/KeybindingsManager.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/tui/Theme.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/tui/ThemeController.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/tui/InteractiveMode.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/cli/CliParse.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/cli/FrontendSelection.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/cli/ListModels.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/cli/StartupTui.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/runtime/AsyncCliRuntime.cpp"));

    const auto ai_sources = target_decl_block(cmake, "cch_ai");
    CHECK(block_mentions(ai_sources, "src/ai/Models.cpp"));
    CHECK_FALSE(block_mentions(ai_sources, "src/ai/ProviderRegistry.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/BoostBeastStreamTransport.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/FakeProvider.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/SseParser.cpp"));
}

TEST_CASE(
    "Agent Core has one authoritative target with one legal Owner edge",
    "[architecture][cmake][issue460]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    const auto agent_core = target_decl_block(cmake, "cch_agent_core");
    REQUIRE_FALSE(agent_core.empty());
    CHECK(target_decl_block(cmake, "cch_agent").empty());
    CHECK(target_decl_block(cmake, "cch_harness").empty());
    CHECK(target_decl_block(cmake, "cch_tools").empty());

    const std::vector<std::string> authoritative_sources{
        "src/agent/Agent.cpp",
        "src/agent/AgentLoop.cpp",
        "src/agent/AgentPolicyAdapters.cpp",
        "src/agent/ToolArgumentPreparation.cpp",
        "src/agent/ToolCallExecutor.cpp",
        "src/harness/AsyncLocalExecutionEnv.cpp",
        "src/harness/RuntimeRoot.cpp",
        "src/harness/ShellResolver.cpp",
        "src/harness/SyncLocalExecutionEnv.cpp",
        "src/harness/WorkspaceFileSystemFdWalk.cpp",
        "src/harness/WorkspaceFileSystemLegacy.cpp",
        "src/harness/WorkspaceFileSystemPi.cpp",
        "src/harness/WorkspaceFileSystemTemp.cpp",
        "src/harness/compaction/Compaction.cpp",
        "src/harness/session/SessionJournal.cpp",
        "src/harness/session/EntrySerializer.cpp",
        "src/harness/session/InMemorySessionStore.cpp",
        "src/harness/session/JsonlSessionStore.cpp",
        "src/harness/session/SessionResume.cpp",
        "src/harness/session/SessionTree.cpp",
        "src/tools/AsyncToolFactories.cpp",
        "src/tools/EditDiff.cpp",
    };
    for (const auto& source : authoritative_sources) {
        CHECK(block_mentions(agent_core, source));
        CHECK(cch::tests::count_occurrences(cmake, source) == 1);
    }

    const auto agent_core_links = depends_section(cmake, "cch_agent_core");
    CHECK(block_mentions(agent_core_links, "cch_ai"));
    CHECK(block_mentions(agent_core_links, "cch_support"));
    CHECK_FALSE(block_mentions(agent_core_links, "cch_coding_agent"));
    CHECK_FALSE(block_mentions(agent_core_links, "cch_tui"));

    const auto coding_agent_links = depends_section(cmake, "cch_coding_agent");
    CHECK(block_mentions(coding_agent_links, "cch_agent_core"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_agent\n"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_harness"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_tools"));
}

TEST_CASE(
    "TUI Toolkit has one authoritative target owning every TUI source exactly once",
    "[architecture][cmake][issue463]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    const auto tui_target = target_decl_block(cmake, "cch_tui");
    REQUIRE_FALSE(tui_target.empty());
    CHECK(block_mentions(tui_target, "ROLE owner"));
    CHECK(block_mentions(tui_target, "OWNER cch_tui"));

    const std::vector<std::string> authoritative_sources{
        "src/tui/Autocomplete.cpp",
        "src/tui/CancellableLoader.cpp",
        "src/tui/Container.cpp",
        "src/tui/Editor.cpp",
        "src/tui/Fuzzy.cpp",
        "src/tui/Image.cpp",
        "src/tui/Input.cpp",
        "src/tui/InputDecoder.cpp",
        "src/tui/Keybindings.cpp",
        "src/tui/KeyboardProtocol.cpp",
        "src/tui/Keys.cpp",
        "src/tui/Loader.cpp",
        "src/tui/Markdown.cpp",
        "src/tui/Overlay.cpp",
        "src/tui/ProcessTerminal.cpp",
        "src/tui/SelectList.cpp",
        "src/tui/SettingsList.cpp",
        "src/tui/TerminalImage.cpp",
        "src/tui/Text.cpp",
        "src/tui/TruncatedText.cpp",
        "src/tui/Tui.cpp",
        "src/tui/UnicodeWidth.cpp",
        "src/tui/Utils.cpp",
        "src/tui/VirtualTerminal.cpp",
    };
    for (const auto& source : authoritative_sources) {
        CHECK(block_mentions(tui_target, source));
        CHECK(cch::tests::count_occurrences(cmake, source) == 1);
    }

    // The authoritative owner's only interface dependency is the pi-neutral
    // support package; terminal, encoding, and execution dependencies stay
    // private. No reverse or cross-Owner capability edge exists.
    const auto tui_links = depends_section(cmake, "cch_tui");
    CHECK(block_mentions(tui_links, "cch_support"));
    CHECK(block_mentions(tui_links, "cch_util"));
    CHECK(block_mentions(tui_links, "md4c::md4c"));
    CHECK(block_mentions(tui_links, "utf8proc::utf8proc"));
    CHECK_FALSE(block_mentions(tui_links, "cch_ai"));
    CHECK_FALSE(block_mentions(tui_links, "cch_agent_core"));
    CHECK_FALSE(block_mentions(tui_links, "cch_harness"));
    CHECK_FALSE(block_mentions(tui_links, "cch_tools"));
    CHECK_FALSE(block_mentions(tui_links, "cch_coding_agent"));

    // TUI Owner Interfaces use one canonical support root: the support
    // dependency is interface-visible while every other dependency is private.
    const auto tui_block = target_decl_block(cmake, "cch_tui");
    const auto interface_start = tui_block.find("INTERFACE_DEPENDS");
    CHECK(interface_start != std::string::npos);
    const auto interface_section = tui_block.substr(interface_start);
    CHECK(interface_section.find("cch_support") != std::string::npos);
    CHECK_FALSE(interface_section.find("cch_util") != std::string::npos);

    // The coding-agent TUI configuration (theme/keybindings) is not a TUI
    // Toolkit capability: it lives in the one repository-private
    // cch_coding_agent library that composes over cch_tui (#468).
    const auto coding_agent_block = target_decl_block(cmake, "cch_coding_agent");
    CHECK(block_mentions(coding_agent_block, "src/coding_agent/tui/Theme.cpp"));
    CHECK(block_mentions(coding_agent_block, "src/coding_agent/tui/KeybindingsManager.cpp"));
    CHECK(block_mentions(coding_agent_block, "src/coding_agent/tui/ThemeController.cpp"));
    CHECK(block_mentions(depends_section(cmake, "cch_coding_agent"), "cch_tui"));
}

TEST_CASE(
    "one repository-private cch_coding_agent library owns all coding-agent composition",
    "[architecture][cmake][issue468]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    // One authoritative compiled library carries the Owner role for every
    // coding-agent composition production source.
    const auto coding_agent = target_decl_block(cmake, "cch_coding_agent");
    REQUIRE_FALSE(coding_agent.empty());
    CHECK(block_mentions(coding_agent, "ROLE owner"));
    CHECK(block_mentions(coding_agent, "OWNER cch_coding_agent"));

    // The pre-contraction split targets and the transitional executable
    // aggregate are gone: no second target may own composition sources.
    CHECK(target_decl_block(cmake, "cch_coding_agent_core").empty());
    CHECK(target_decl_block(cmake, "cch_coding_agent_tui").empty());
    CHECK(target_decl_block(cmake, "cch_coding_agent_runtime").empty());
    CHECK(target_decl_block(cmake, "cch_coding_agent_interactive").empty());
    CHECK(target_decl_block(cmake, "cch_cli").empty());
    CHECK(target_decl_block(cmake, "cpp_harness_lib").empty());

    // Models Runtime, Session, Runtime, Native TUI composition, and CLI
    // composition sources each appear in the one library exactly once.
    const std::vector<std::string> authoritative_sources{
        "src/coding_agent/AgentConfigDir.cpp",
        "src/coding_agent/AgentSession.cpp",
        "src/coding_agent/AuthStorage.cpp",
        "src/coding_agent/GitIgnoreMatcher.cpp",
        "src/coding_agent/ImageInput.cpp",
        "src/coding_agent/ModelConfig.cpp",
        "src/coding_agent/ModelResolver.cpp",
        "src/coding_agent/ModelRuntime.cpp",
        "src/coding_agent/ProjectResourceLoader.cpp",
        "src/coding_agent/ProjectResources.cpp",
        "src/coding_agent/ProjectTrust.cpp",
        "src/coding_agent/PromptTemplateLoader.cpp",
        "src/coding_agent/ProviderComposer.cpp",
        "src/coding_agent/RuntimeApiKeyOverlay.cpp",
        "src/coding_agent/SessionDiscovery.cpp",
        "src/coding_agent/SessionPathPolicy.cpp",
        "src/coding_agent/SettingsManager.cpp",
        "src/coding_agent/SkillFormatting.cpp",
        "src/coding_agent/SkillFrontmatterParser.cpp",
        "src/coding_agent/SkillLoader.cpp",
        "src/coding_agent/prompt/BuiltinSlashCommands.cpp",
        "src/coding_agent/prompt/PromptExpansion.cpp",
        "src/coding_agent/prompt/PromptTemplateExpander.cpp",
        "src/coding_agent/prompt/SystemPromptBuilder.cpp",
        "src/coding_agent/runtime/AgentSessionRuntime.cpp",
        "src/coding_agent/runtime/AsyncCliRuntime.cpp",
        "src/coding_agent/runtime/AuthGuidanceStream.cpp",
        "src/coding_agent/runtime/LocalUserShell.cpp",
        "src/coding_agent/runtime/SessionEventCommitment.cpp",
        "src/coding_agent/runtime/SessionFactory.cpp",
        "src/coding_agent/runtime/SessionFork.cpp",
        "src/coding_agent/runtime/SessionLifecycle.cpp",
        "src/coding_agent/runtime/SessionPersistence.cpp",
        "src/coding_agent/runtime/UserBashOutputAccumulator.cpp",
        "src/coding_agent/tui/AssistantMessageComponent.cpp",
        "src/coding_agent/tui/BashExecutionComponent.cpp",
        "src/coding_agent/tui/ChatContainer.cpp",
        "src/coding_agent/tui/ClipboardWrite.cpp",
        "src/coding_agent/tui/DiffRenderer.cpp",
        "src/coding_agent/tui/ExternalEditor.cpp",
        "src/coding_agent/tui/Footer.cpp",
        "src/coding_agent/tui/FooterDataProvider.cpp",
        "src/coding_agent/tui/InteractiveMode.cpp",
        "src/coding_agent/tui/KeybindingHints.cpp",
        "src/coding_agent/tui/KeybindingsManager.cpp",
        "src/coding_agent/tui/LoadedResources.cpp",
        "src/coding_agent/tui/LoginDialog.cpp",
        "src/coding_agent/tui/LoginPresentation.cpp",
        "src/coding_agent/tui/ModelSelector.cpp",
        "src/coding_agent/tui/OAuthSelector.cpp",
        "src/coding_agent/tui/OpenBrowser.cpp",
        "src/coding_agent/tui/ReloadBox.cpp",
        "src/coding_agent/tui/ScopedModelsSelector.cpp",
        "src/coding_agent/tui/SessionSelector.cpp",
        "src/coding_agent/tui/SessionSelectorSearch.cpp",
        "src/coding_agent/tui/SettingsSelector.cpp",
        "src/coding_agent/tui/StatusIndicator.cpp",
        "src/coding_agent/tui/StringListSelector.cpp",
        "src/coding_agent/tui/Theme.cpp",
        "src/coding_agent/tui/ThemeController.cpp",
        "src/coding_agent/tui/ToolExecutionComponent.cpp",
        "src/coding_agent/tui/TreeSelector.cpp",
        "src/coding_agent/tui/UserMessageComponent.cpp",
        "src/coding_agent/tui/UserMessageSelector.cpp",
        "src/cli/CliParse.cpp",
        "src/cli/FrontendSelection.cpp",
        "src/cli/InitialPrompt.cpp",
        "src/cli/ListModels.cpp",
        "src/cli/PrintMode.cpp",
        "src/cli/SessionFamily.cpp",
        "src/cli/StartupTui.cpp",
    };
    for (const auto& source : authoritative_sources) {
        CHECK(block_mentions(coding_agent, source));
        CHECK(cch::tests::count_occurrences(cmake, source) == 1);
    }

    // The only cross-Owner edges are the legal ones to cch_agent_core,
    // cch_ai, and cch_tui; cch_support is the pi-neutral support package and
    // cch_util remains the temporary legacy alias until #469.
    const auto links = depends_section(cmake, "cch_coding_agent");
    CHECK(block_mentions(links, "cch_agent_core"));
    CHECK(block_mentions(links, "cch_ai"));
    CHECK(block_mentions(links, "cch_tui"));
    CHECK(block_mentions(links, "cch_support"));
    CHECK(block_mentions(links, "cch_util"));
    CHECK(block_mentions(links, "WebP::webpdecoder@webp"));
    CHECK(block_mentions(links, "CLI11::CLI11@cli11"));
    CHECK_FALSE(block_mentions(links, "cch_coding_agent_"));
    CHECK_FALSE(block_mentions(links, "cch_cli"));
    CHECK_FALSE(block_mentions(links, "cch_harness"));
    CHECK_FALSE(block_mentions(links, "cch_tools"));

    // No other Owner or support target depends on the repository-private
    // coding-agent library or on the executable closure.
    for (const auto& other : {"cch_ai", "cch_agent_core", "cch_tui", "cch_util", "cch_support"}) {
        const auto other_links = depends_section(cmake, other);
        CHECK_FALSE(block_mentions(other_links, "cch_coding_agent"));
        CHECK_FALSE(block_mentions(other_links, "cch_cli"));
        CHECK_FALSE(block_mentions(other_links, "cpp_harness"));
    }

    // The executable is a thin closure: one entry-point source and exactly
    // one link edge to the repository-private Owner library.
    const auto executable = target_decl_block(cmake, "cpp_harness");
    REQUIRE_FALSE(executable.empty());
    CHECK(block_mentions(executable, "KIND executable"));
    CHECK(block_mentions(executable, "OWNER cch_coding_agent"));
    CHECK(block_mentions(executable, "src/main.cpp"));
    CHECK(cch::tests::count_occurrences(executable, "src/") == 1);
    const auto executable_links = depends_section(cmake, "cpp_harness");
    CHECK(block_mentions(executable_links, "cch_coding_agent"));
    CHECK_FALSE(block_mentions(executable_links, "cch_agent_core"));
    CHECK_FALSE(block_mentions(executable_links, "cch_ai"));
    CHECK_FALSE(block_mentions(executable_links, "cch_tui"));
    CHECK_FALSE(block_mentions(executable_links, "CLI11"));
}

TEST_CASE("CMake target links follow the package dependency direction", "[architecture][cmake][issue58]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    const auto tui_links = depends_section(cmake, "cch_tui");
    CHECK(block_mentions(tui_links, "cch_util"));
    CHECK_FALSE(block_mentions(tui_links, "cch_ai"));
    CHECK_FALSE(block_mentions(tui_links, "cch_agent"));
    CHECK_FALSE(block_mentions(tui_links, "cch_harness"));
    CHECK_FALSE(block_mentions(tui_links, "cch_tools"));
    CHECK_FALSE(block_mentions(tui_links, "cch_coding_agent"));

    const auto ai_links = depends_section(cmake, "cch_ai");
    CHECK(block_mentions(ai_links, "cch_util"));
    CHECK_FALSE(block_mentions(ai_links, "cch_agent"));
    CHECK_FALSE(block_mentions(ai_links, "cch_harness"));
    CHECK_FALSE(block_mentions(ai_links, "cch_tools"));
    CHECK_FALSE(block_mentions(ai_links, "cch_tui"));
    CHECK_FALSE(block_mentions(ai_links, "cch_coding_agent"));

    const auto agent_core_links = depends_section(cmake, "cch_agent_core");
    CHECK(block_mentions(agent_core_links, "cch_ai"));
    CHECK(block_mentions(agent_core_links, "cch_support"));
    CHECK(block_mentions(agent_core_links, "cch_util"));
    CHECK_FALSE(block_mentions(agent_core_links, "cch_tui"));
    CHECK_FALSE(block_mentions(agent_core_links, "cch_coding_agent"));

    // The one repository-private cch_coding_agent library (#468) carries only
    // the legal cross-Owner edges; CLI composition folded into it, so there
    // is no separate cch_cli link layer any more.
    const auto coding_agent_links = depends_section(cmake, "cch_coding_agent");
    CHECK(block_mentions(coding_agent_links, "cch_agent_core"));
    CHECK(block_mentions(coding_agent_links, "cch_ai"));
    CHECK(block_mentions(coding_agent_links, "cch_tui"));
    CHECK(block_mentions(coding_agent_links, "cch_support"));
    CHECK(block_mentions(coding_agent_links, "cch_util"));
    CHECK(block_mentions(coding_agent_links, "WebP::webpdecoder"));
    CHECK(block_mentions(coding_agent_links, "CLI11::CLI11"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_harness"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_tools"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_coding_agent_"));
    CHECK_FALSE(block_mentions(coding_agent_links, "cch_cli"));

    // The executable compiles only main.cpp and closes over the one
    // repository-private Owner library; the shared CLI/runtime sources have
    // exactly one owner (cch_coding_agent).
    const auto executable_block = target_decl_block(cmake, "cpp_harness");
    CHECK(block_mentions(executable_block, "src/main.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/CliParse.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/FrontendSelection.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/ListModels.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/StartupTui.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/coding_agent/runtime/AsyncCliRuntime.cpp"));
    const auto executable_links = depends_section(cmake, "cpp_harness");
    CHECK(block_mentions(executable_links, "cch_coding_agent"));
    CHECK_FALSE(block_mentions(executable_links, "cch_cli"));

    // The package-aligned test shards (build-performance-plan Stage 4) split
    // tests along package boundaries: each shard is its own executable that
    // registers with ctest, and `cpp_harness_tests` is the aggregate target
    // that builds every shard. No shard recompiles the shared CLI/runtime
    // sources; they link the repository-private cch_coding_agent library.
    const auto aggregate = cmake_command_block(cmake, "add_custom_target(cpp_harness_tests");
    CHECK(block_mentions(aggregate, "DEPENDS"));
    CHECK(block_mentions(aggregate, "cch_tests_util"));
    CHECK(block_mentions(aggregate, "cch_tests_tui"));
    CHECK(block_mentions(aggregate, "cch_tests_ai"));
    CHECK(block_mentions(aggregate, "cch_tests_agent"));
    CHECK(block_mentions(aggregate, "cch_tests_harness_tools"));
    CHECK(block_mentions(aggregate, "cch_tests_coding_agent"));
    CHECK(block_mentions(aggregate, "cch_tests_coding_agent_interactive"));
    CHECK(block_mentions(aggregate, "cch_tests_cli_arch"));

    const std::vector<std::string> shards{
        "cch_tests_util", "cch_tests_tui", "cch_tests_ai", "cch_tests_agent",
        "cch_tests_harness_tools", "cch_tests_coding_agent",
        "cch_tests_coding_agent_interactive", "cch_tests_cli_arch",
    };
    for (const auto& shard : shards) {
        const auto shard_sources = cmake_command_block(
            cmake, "add_executable(" + shard);
        CHECK_FALSE(block_mentions(shard_sources, "src/cli/CliParse.cpp"));
        CHECK_FALSE(block_mentions(shard_sources, "src/cli/FrontendSelection.cpp"));
        CHECK_FALSE(block_mentions(shard_sources, "src/cli/ListModels.cpp"));
        CHECK_FALSE(block_mentions(shard_sources, "src/cli/StartupTui.cpp"));
        CHECK_FALSE(block_mentions(shard_sources, "src/coding_agent/runtime/AsyncCliRuntime.cpp"));
        // Every package-aligned shard now registers its cases through
        // catch_discover_tests; the legacy monolithic add_test entry is gone.
        CHECK(block_mentions(cmake, "catch_discover_tests(" + shard));
    }

    CHECK(test_modules_in(
              cmake_command_block(cmake, "add_executable(cch_tests_util")) ==
          std::vector<std::string>{"tests/util"});
    CHECK(test_modules_in(
              cmake_command_block(cmake, "add_executable(cch_tests_tui")) ==
          std::vector<std::string>{"tests/tui"});
    CHECK(test_modules_in(
              cmake_command_block(cmake, "add_executable(cch_tests_ai")) ==
          std::vector<std::string>{"tests/ai"});
    CHECK(test_modules_in(
              cmake_command_block(cmake, "add_executable(cch_tests_agent")) ==
          std::vector<std::string>{"tests/agent"});
    CHECK(test_modules_in(cmake_command_block(
              cmake, "add_executable(cch_tests_harness_tools")) ==
          (std::vector<std::string>{"tests/harness", "tests/tools"}));
    CHECK(test_modules_in(
              cmake_command_block(cmake, "add_executable(cch_tests_cli_arch")) ==
          (std::vector<std::string>{"tests/architecture", "tests/cli"}));

    // The coding-agent package splits on the tui subdirectory: the
    // core/runtime shard owns tests/coding_agent/ and /runtime/ but never the
    // interactive tui tests, and the interactive shard owns only /tui/.
    const auto coding_agent_sources = cmake_command_block(
        cmake, "add_executable(cch_tests_coding_agent\n");
    CHECK(test_modules_in(coding_agent_sources) ==
          std::vector<std::string>{"tests/coding_agent"});
    const auto coding_agent_children_set = coding_agent_children(coding_agent_sources);
    CHECK(std::find(coding_agent_children_set.begin(), coding_agent_children_set.end(), "runtime") !=
          coding_agent_children_set.end());
    CHECK(std::find(coding_agent_children_set.begin(), coding_agent_children_set.end(), "tui") ==
          coding_agent_children_set.end());
    const auto interactive_sources = cmake_command_block(
        cmake, "add_executable(cch_tests_coding_agent_interactive");
    CHECK(coding_agent_children(interactive_sources) ==
          std::vector<std::string>{"tui"});

    // Only the CLI/architecture shard launches the built binary
    // (CliSmokeTest); it carries the CCH_BINARY definition and a build
    // dependency on cpp_harness. Every coding-agent consumer shard links the
    // one repository-private cch_coding_agent library (#468); the interactive
    // shard drives the in-process CLI seam (CliRunFixture) through it.
    const auto cli_arch_sources = cmake_command_block(
        cmake, "add_executable(cch_tests_cli_arch");
    const auto cli_arch_links = cmake_command_block(
        cmake, "target_link_libraries(cch_tests_cli_arch");
    CHECK(block_mentions(cli_arch_sources, "tests/cli/CliSmokeTest.cpp"));
    CHECK(block_mentions(cli_arch_links, "cch_coding_agent"));
    CHECK(block_mentions(cli_arch_links, "CLI11::CLI11"));
    CHECK(block_mentions(cmake, "CCH_BINARY="));
    const auto coding_agent_shard_links = cmake_command_block(
        cmake, "target_link_libraries(cch_tests_coding_agent\n");
    CHECK(block_mentions(coding_agent_shard_links, "cch_coding_agent"));
    const auto interactive_shard_links = cmake_command_block(
        cmake, "target_link_libraries(cch_tests_coding_agent_interactive");
    CHECK(block_mentions(interactive_shard_links, "cch_coding_agent"));
    // This shard includes Asio directly. Keep it on vcpkg's Boost headers so
    // its co_spawn frames cannot mix with the interactive composition's
    // version.
    CHECK(block_mentions(interactive_shard_links, "Boost::headers"));
}

TEST_CASE(
    "image input and transcript composition stay in their owning private packages",
    "[architecture][cmake][issue63]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");
    const auto tui_sources = target_decl_block(cmake, "cch_tui");
    const auto coding_agent_sources = target_decl_block(cmake, "cch_coding_agent");

    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/ImageInput.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/cli/InitialPrompt.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/tui/InteractiveMode.cpp"));
    CHECK(block_mentions(coding_agent_sources, "src/coding_agent/tui/ChatContainer.cpp"));
    CHECK_FALSE(block_mentions(tui_sources, "ImageInput"));
    CHECK_FALSE(block_mentions(tui_sources, "ClipboardReader"));
    CHECK_FALSE(block_mentions(tui_sources, "ChatContainer.cpp"));
    const auto coding_agent_links = depends_section(cmake, "cch_coding_agent");
    const auto tui_links = depends_section(cmake, "cch_tui");
    CHECK(block_mentions(coding_agent_links, "WebP::webpdecoder"));
    CHECK_FALSE(block_mentions(tui_links, "WebP::webpdecoder"));
}

TEST_CASE("provider implementations stay below the AI package boundary", "[architecture][cmake]") {
    const auto provider_root = std::filesystem::path(CCH_SOURCE_DIR) / "src" / "ai" / "providers";
    const auto files = files_under(provider_root);
    REQUIRE_FALSE(files.empty());

    for (const auto& file : files) {
        const auto text = read_text(file);
        CHECK(text.find("AsyncCliRuntime") == std::string::npos);
        CHECK(text.find("ToolFactories") == std::string::npos);
        CHECK(text.find("cch/tools") == std::string::npos);
        CHECK(text.find("cch/harness") == std::string::npos);
        CHECK(text.find("../tools") == std::string::npos);
        CHECK(text.find("../harness") == std::string::npos);
    }
}
