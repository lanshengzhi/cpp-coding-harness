#include "../../third_party/catch2/catch_test_macros.hpp"

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

} // namespace

TEST_CASE("CMake declares pi package-style targets", "[architecture][cmake][issue57]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    CHECK(block_mentions(cmake, "add_library(cch_util"));
    CHECK(block_mentions(cmake, "add_library(cch_tui"));
    const auto tui_sources = cmake_command_block(cmake, "add_library(cch_tui");
    CHECK(block_mentions(tui_sources, "src/tui/CancellableLoader.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Image.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Input.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/InputDecoder.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Keybindings.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Loader.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Markdown.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/SelectList.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/SettingsList.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/TerminalImage.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Text.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/Tui.cpp"));
    CHECK(block_mentions(tui_sources, "src/tui/VirtualTerminal.cpp"));
    CHECK(block_mentions(cmake, "add_library(cch_coding_agent_tui"));
    const auto coding_agent_tui_sources = cmake_command_block(cmake, "add_library(cch_coding_agent_tui");
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/KeybindingCatalog.cpp"));
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/KeybindingHelp.cpp"));
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/Theme.cpp"));
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/ThemeCatalog.cpp"));
    CHECK(block_mentions(coding_agent_tui_sources, "src/coding_agent/tui/ThemeSettings.cpp"));
    CHECK(block_mentions(cmake, "add_library(cch_ai"));
    CHECK(block_mentions(cmake, "add_library(cch_agent"));
    const auto agent_sources = cmake_command_block(cmake, "add_library(cch_agent");
    CHECK(block_mentions(agent_sources, "src/agent/Agent.cpp"));
    CHECK(block_mentions(agent_sources, "src/agent/AgentLoop.cpp"));
    CHECK(block_mentions(cmake, "add_library(cch_harness"));
    CHECK(block_mentions(cmake, "add_library(cch_tools"));
    CHECK(block_mentions(cmake, "add_library(cch_coding_agent_runtime"));

    const auto ai_sources = cmake_command_block(cmake, "add_library(cch_ai");
    CHECK(block_mentions(ai_sources, "src/ai/ProviderRegistry.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/BoostBeastStreamTransport.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/FakeChatClient.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/OpenAIChatClient.cpp"));
    CHECK(block_mentions(ai_sources, "src/ai/providers/SseParser.cpp"));
}

TEST_CASE("CMake target links follow the package dependency direction", "[architecture][cmake]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    const auto tui_links = cmake_command_block(cmake, "target_link_libraries(cch_tui");
    CHECK(block_mentions(tui_links, "cch_util"));
    CHECK_FALSE(block_mentions(tui_links, "cch_ai"));
    CHECK_FALSE(block_mentions(tui_links, "cch_agent"));
    CHECK_FALSE(block_mentions(tui_links, "cch_harness"));
    CHECK_FALSE(block_mentions(tui_links, "cch_tools"));
    CHECK_FALSE(block_mentions(tui_links, "cch_coding_agent_runtime"));

    const auto coding_agent_tui_links = cmake_command_block(cmake, "target_link_libraries(cch_coding_agent_tui");
    CHECK(block_mentions(coding_agent_tui_links, "cch_tui"));
    CHECK(block_mentions(coding_agent_tui_links, "cch_util"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_agent"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_harness"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_tools"));
    CHECK_FALSE(block_mentions(coding_agent_tui_links, "cch_coding_agent_runtime"));

    const auto ai_links = cmake_command_block(cmake, "target_link_libraries(cch_ai");
    CHECK(block_mentions(ai_links, "cch_util"));
    CHECK_FALSE(block_mentions(ai_links, "cch_agent"));
    CHECK_FALSE(block_mentions(ai_links, "cch_harness"));
    CHECK_FALSE(block_mentions(ai_links, "cch_tools"));
    CHECK_FALSE(block_mentions(ai_links, "cch_coding_agent_runtime"));

    const auto agent_links = cmake_command_block(cmake, "target_link_libraries(cch_agent");
    CHECK(block_mentions(agent_links, "cch_ai"));
    CHECK(block_mentions(agent_links, "cch_util"));
    CHECK_FALSE(block_mentions(agent_links, "cch_harness"));
    CHECK_FALSE(block_mentions(agent_links, "cch_tools"));
    CHECK_FALSE(block_mentions(agent_links, "cch_coding_agent_runtime"));

    const auto harness_links = cmake_command_block(cmake, "target_link_libraries(cch_harness");
    CHECK(block_mentions(harness_links, "cch_ai"));
    CHECK(block_mentions(harness_links, "cch_util"));
    CHECK_FALSE(block_mentions(harness_links, "cch_agent"));
    CHECK_FALSE(block_mentions(harness_links, "cch_tools"));
    CHECK_FALSE(block_mentions(harness_links, "cch_coding_agent_runtime"));

    const auto tools_links = cmake_command_block(cmake, "target_link_libraries(cch_tools");
    CHECK(block_mentions(tools_links, "cch_agent"));
    CHECK(block_mentions(tools_links, "cch_harness"));
    CHECK_FALSE(block_mentions(tools_links, "cch_coding_agent_runtime"));

    const auto runtime_links = cmake_command_block(cmake, "target_link_libraries(cch_coding_agent_runtime");
    CHECK(block_mentions(runtime_links, "cch_agent"));
    CHECK(block_mentions(runtime_links, "cch_harness"));
    CHECK(block_mentions(runtime_links, "cch_tools"));
    CHECK(block_mentions(runtime_links, "cch_ai"));
    CHECK_FALSE(block_mentions(runtime_links, "cch_tui"));
    CHECK_FALSE(block_mentions(runtime_links, "cch_coding_agent_tui"));
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
