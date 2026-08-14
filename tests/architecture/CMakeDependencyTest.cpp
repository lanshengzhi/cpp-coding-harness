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
    CHECK(block_mentions(cmake, "TARGET cch_coding_agent_tui\n"));
    const auto coding_agent_tui_sources = target_decl_block(cmake, "cch_coding_agent_tui");
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/KeybindingsManager.cpp"));
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/Theme.cpp"));
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/ThemeController.cpp"));
    CHECK(block_mentions(cmake, "TARGET cch_coding_agent_interactive\n"));
    const auto interactive_sources = target_decl_block(cmake, "cch_coding_agent_interactive");
    CHECK(block_mentions(interactive_sources, "src/coding_agent/tui/InteractiveMode.cpp"));
    CHECK(block_mentions(cmake, "TARGET cch_ai\n"));
    CHECK(block_mentions(cmake, "TARGET cch_agent\n"));
    const auto agent_sources = target_decl_block(cmake, "cch_agent");
    CHECK(block_mentions(agent_sources, "src/agent/Agent.cpp"));
    CHECK(block_mentions(agent_sources, "src/agent/AgentLoop.cpp"));
    CHECK(block_mentions(cmake, "TARGET cch_harness\n"));
    const auto harness_sources = target_decl_block(cmake, "cch_harness");
    CHECK(block_mentions(harness_sources, "src/harness/ShellResolver.cpp"));
    CHECK(block_mentions(cmake, "TARGET cch_tools\n"));
    CHECK(block_mentions(cmake, "TARGET cch_coding_agent_core\n"));
    const auto core_sources = target_decl_block(cmake, "cch_coding_agent_core");
    CHECK(block_mentions(core_sources, "src/coding_agent/ModelRuntime.cpp"));
    CHECK(block_mentions(core_sources, "src/coding_agent/ModelConfig.cpp"));
    CHECK(block_mentions(core_sources, "src/coding_agent/ProviderComposer.cpp"));
    CHECK(block_mentions(cmake, "TARGET cch_coding_agent_runtime\n"));
    CHECK(block_mentions(cmake, "TARGET cch_cli\n"));
    const auto cli_sources = target_decl_block(cmake, "cch_cli");
    CHECK(block_mentions(cli_sources, "src/cli/CliParse.cpp"));
    CHECK(block_mentions(cli_sources, "src/cli/FrontendSelection.cpp"));
    CHECK(block_mentions(cli_sources, "src/cli/ListModels.cpp"));
    CHECK(block_mentions(cli_sources, "src/cli/StartupTui.cpp"));
    CHECK(block_mentions(cli_sources, "src/coding_agent/runtime/AsyncCliRuntime.cpp"));

    const auto ai_sources = target_decl_block(cmake, "cch_ai");
    CHECK(block_mentions(ai_sources, "src/ai/Models.cpp"));
    CHECK_FALSE(block_mentions(ai_sources, "src/ai/ProviderRegistry.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/BoostBeastStreamTransport.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/FakeProvider.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/SseParser.cpp"));
}

TEST_CASE("CMake target links follow the package dependency direction", "[architecture][cmake][issue58]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    const auto tui_links = depends_section(cmake, "cch_tui");
    CHECK(block_mentions(tui_links, "cch_util"));
    CHECK_FALSE(block_mentions(tui_links, "cch_ai"));
    CHECK_FALSE(block_mentions(tui_links, "cch_agent"));
    CHECK_FALSE(block_mentions(tui_links, "cch_harness"));
    CHECK_FALSE(block_mentions(tui_links, "cch_tools"));
    CHECK_FALSE(block_mentions(tui_links, "cch_coding_agent_core"));
    CHECK_FALSE(block_mentions(tui_links, "cch_coding_agent_runtime"));

    const auto coding_agent_tui_links = depends_section(cmake, "cch_coding_agent_tui");
    CHECK(block_mentions(coding_agent_tui_links, "cch_tui"));
    CHECK(block_mentions(coding_agent_tui_links, "cch_util"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_agent"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_harness"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_tools"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_coding_agent_core"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_coding_agent_runtime"));

    const auto interactive_links = depends_section(cmake, "cch_coding_agent_interactive");
    CHECK(block_mentions(interactive_links, "cch_coding_agent_runtime"));
    CHECK(block_mentions(interactive_links, "cch_coding_agent_tui"));
    CHECK(block_mentions(interactive_links, "cch_tui"));
    CHECK(block_mentions(interactive_links, "cch_util"));

    const auto ai_links = depends_section(cmake, "cch_ai");
    CHECK(block_mentions(ai_links, "cch_util"));
    CHECK_FALSE(block_mentions(ai_links, "cch_agent"));
    CHECK_FALSE(block_mentions(ai_links, "cch_harness"));
    CHECK_FALSE(block_mentions(ai_links, "cch_tools"));
    CHECK_FALSE(block_mentions(ai_links, "cch_coding_agent_core"));
    CHECK_FALSE(block_mentions(ai_links, "cch_coding_agent_runtime"));

    const auto agent_links = depends_section(cmake, "cch_agent");
    CHECK(block_mentions(agent_links, "cch_coding_agent_core"));
    CHECK(block_mentions(agent_links, "cch_ai"));
    CHECK(block_mentions(agent_links, "cch_util"));
    CHECK_FALSE(block_mentions(agent_links, "cch_harness"));
    CHECK_FALSE(block_mentions(agent_links, "cch_tools"));
    CHECK_FALSE(block_mentions(agent_links, "cch_coding_agent_runtime"));

    const auto harness_links = depends_section(cmake, "cch_harness");
    CHECK(block_mentions(harness_links, "cch_ai"));
    CHECK(block_mentions(harness_links, "cch_util"));
    CHECK_FALSE(block_mentions(harness_links, "cch_agent"));
    CHECK_FALSE(block_mentions(harness_links, "cch_tools"));
    CHECK_FALSE(block_mentions(harness_links, "cch_coding_agent_core"));
    CHECK_FALSE(block_mentions(harness_links, "cch_coding_agent_runtime"));

    const auto tools_links = depends_section(cmake, "cch_tools");
    CHECK(block_mentions(tools_links, "cch_agent"));
    CHECK(block_mentions(tools_links, "cch_harness"));
    CHECK_FALSE(block_mentions(tools_links, "cch_coding_agent_core"));
    CHECK_FALSE(block_mentions(tools_links, "cch_coding_agent_runtime"));

    const auto runtime_links = depends_section(cmake, "cch_coding_agent_runtime");
    CHECK(block_mentions(runtime_links, "cch_agent"));
    CHECK(block_mentions(runtime_links, "cch_harness"));
    CHECK(block_mentions(runtime_links, "cch_tools"));
    CHECK(block_mentions(runtime_links, "cch_coding_agent_core"));
    CHECK(block_mentions(runtime_links, "cch_ai"));
    CHECK(block_mentions(runtime_links, "WebP::webpdecoder"));
    CHECK_FALSE(block_mentions(runtime_links, "cch_tui"));
    CHECK_FALSE(block_mentions(runtime_links, "cch_coding_agent_tui"));
    CHECK_FALSE(block_mentions(runtime_links, "cch_coding_agent_interactive"));

    const auto cli_links = depends_section(cmake, "cch_cli");
    CHECK(block_mentions(cli_links, "cch_coding_agent_interactive"));
    CHECK(block_mentions(cli_links, "CLI11::CLI11"));
    CHECK_FALSE(block_mentions(cli_links, "cch_coding_agent_runtime"));
    CHECK_FALSE(block_mentions(cli_links, "cch_coding_agent_tui"));
    CHECK_FALSE(block_mentions(cli_links, "cch_tui"));

    // The executable compiles only main.cpp and links the authoritative CLI
    // owner; the five shared sources have exactly one owner (cch_cli).
    const auto executable_block = target_decl_block(cmake, "cpp_harness");
    CHECK_FALSE(block_mentions(executable_block, "src/cli/CliParse.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/FrontendSelection.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/ListModels.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/cli/StartupTui.cpp"));
    CHECK_FALSE(block_mentions(executable_block, "src/coding_agent/runtime/AsyncCliRuntime.cpp"));
    CHECK(block_mentions(executable_block, "cch_cli"));

    // The package-aligned test shards (build-performance-plan Stage 4) split
    // tests along package boundaries: each shard is its own executable that
    // registers with ctest, and `cpp_harness_tests` is the aggregate target
    // that builds every shard. No shard recompiles the shared CLI/runtime
    // sources; they link the authoritative cch_cli owner instead.
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
    // dependency on cpp_harness, and the interactive shard drives the
    // in-process CLI seam (CliRunFixture) so it links cch_cli too.
    const auto cli_arch_sources = cmake_command_block(
        cmake, "add_executable(cch_tests_cli_arch");
    const auto cli_arch_links = cmake_command_block(
        cmake, "target_link_libraries(cch_tests_cli_arch");
    CHECK(block_mentions(cli_arch_sources, "tests/cli/CliSmokeTest.cpp"));
    CHECK(block_mentions(cli_arch_links, "cch_cli"));
    CHECK(block_mentions(cli_arch_links, "CLI11::CLI11"));
    CHECK(block_mentions(cmake, "CCH_BINARY="));
    const auto interactive_shard_links = cmake_command_block(
        cmake, "target_link_libraries(cch_tests_coding_agent_interactive");
    CHECK(block_mentions(interactive_shard_links, "cch_cli"));
    CHECK(block_mentions(interactive_shard_links, "cch_coding_agent_interactive"));
    // This shard includes Asio directly. Keep it on vcpkg's Boost headers so
    // its co_spawn frames cannot mix with the interactive library's version.
    CHECK(block_mentions(interactive_shard_links, "Boost::headers"));
}

TEST_CASE(
    "image input and transcript composition stay in their owning private packages",
    "[architecture][cmake][issue63]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");
    const auto tui_sources = target_decl_block(cmake, "cch_tui");
    const auto runtime_sources = target_decl_block(cmake, "cch_coding_agent_runtime");
    const auto interactive_sources = target_decl_block(cmake, "cch_coding_agent_interactive");

    CHECK(block_mentions(runtime_sources, "src/coding_agent/ImageInput.cpp"));
    CHECK(block_mentions(runtime_sources, "src/cli/InitialPrompt.cpp"));
    CHECK(block_mentions(interactive_sources, "src/coding_agent/tui/InteractiveMode.cpp"));
    CHECK(block_mentions(interactive_sources, "src/coding_agent/tui/ChatContainer.cpp"));
    CHECK_FALSE(block_mentions(tui_sources, "ImageInput"));
    CHECK_FALSE(block_mentions(tui_sources, "ClipboardReader"));
    CHECK_FALSE(block_mentions(tui_sources, "ChatContainer.cpp"));
    const auto runtime_links = depends_section(cmake, "cch_coding_agent_runtime");
    const auto tui_links = depends_section(cmake, "cch_tui");
    CHECK(block_mentions(runtime_links, "WebP::webpdecoder"));
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
