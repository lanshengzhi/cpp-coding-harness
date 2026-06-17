#include "../../third_party/catch2/catch_test_macros.hpp"

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

std::string target_link_block(const std::string& cmake, const std::string& target) {
    const auto needle = "target_link_libraries(" + target;
    const auto begin = cmake.find(needle);
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = cmake.find("\n)", begin);
    if (end == std::string::npos) {
        return cmake.substr(begin);
    }
    return cmake.substr(begin, end - begin);
}

std::string add_library_block(const std::string& cmake, const std::string& target) {
    const auto needle = "add_library(" + target;
    const auto begin = cmake.find(needle);
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = cmake.find("\n)", begin);
    if (end == std::string::npos) {
        return cmake.substr(begin);
    }
    return cmake.substr(begin, end - begin);
}

std::vector<std::filesystem::path> files_under(std::initializer_list<std::string> roots) {
    std::vector<std::filesystem::path> files;
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    for (const auto& root_name : roots) {
        const auto root = source_root / root_name;
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
    }
    return files;
}

bool contains_any(const std::string& text, std::initializer_list<std::string> needles) {
    for (const auto& needle : needles) {
        if (text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("CMake targets follow package dependency direction", "[architecture][u2]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    CHECK(cmake.find("add_library(cch_util") != std::string::npos);
    CHECK(cmake.find("add_library(cch_ai") != std::string::npos);
    CHECK(cmake.find("add_library(cch_agent") != std::string::npos);
    CHECK(cmake.find("add_library(cch_harness") != std::string::npos);
    CHECK(cmake.find("add_library(cch_tools") != std::string::npos);
    CHECK(cmake.find("add_library(cch_coding_agent_runtime") != std::string::npos);

    const auto ai_links = target_link_block(cmake, "cch_ai");
    CHECK_FALSE(ai_links.empty());
    CHECK(ai_links.find("cch_agent") == std::string::npos);
    CHECK(ai_links.find("cch_harness") == std::string::npos);
    CHECK(ai_links.find("cch_tools") == std::string::npos);
    CHECK(ai_links.find("cch_coding_agent_runtime") == std::string::npos);

    const auto agent_links = target_link_block(cmake, "cch_agent");
    CHECK_FALSE(agent_links.empty());
    CHECK(agent_links.find("cch_coding_agent_runtime") == std::string::npos);
    CHECK(agent_links.find("cch_tools") == std::string::npos);

    const auto runtime_links = target_link_block(cmake, "cch_coding_agent_runtime");
    CHECK_FALSE(runtime_links.empty());
    CHECK(runtime_links.find("cch_agent") != std::string::npos);
    CHECK(runtime_links.find("cch_harness") != std::string::npos);
    CHECK(runtime_links.find("cch_tools") != std::string::npos);

    const auto aggregate = add_library_block(cmake, "cpp_harness_lib");
    CHECK(aggregate.find("INTERFACE") != std::string::npos);
    CHECK(aggregate.find("src/agent/AgentLoop.cpp") == std::string::npos);
}

TEST_CASE("provider implementations compile under the ai target", "[architecture][u2]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");
    const auto ai_sources = add_library_block(cmake, "cch_ai");
    REQUIRE_FALSE(ai_sources.empty());

    CHECK(ai_sources.find("src/ai/providers/BoostBeastStreamTransport.cpp") != std::string::npos);
    CHECK(ai_sources.find("src/ai/providers/OpenAIChatClient.cpp") != std::string::npos);
    CHECK(ai_sources.find("src/ai/providers/SseParser.cpp") != std::string::npos);

    const auto runtime_sources = add_library_block(cmake, "cch_coding_agent_runtime");
    CHECK(runtime_sources.find("src/ai/providers/") == std::string::npos);
}

TEST_CASE("source includes do not cross forbidden package layers", "[architecture][u2]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);

    for (const auto& file : files_under({"include/cch/ai", "src/ai"})) {
        if (file.extension() != ".hpp" && file.extension() != ".cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK_FALSE(contains_any(text, {
            "cch/agent/",
            "cch/harness/",
            "cch/tools/",
            "AsyncCliRuntime",
            "../agent/",
            "../harness/",
            "../tools/",
            "../../agent/",
            "../../harness/",
            "../../tools/",
        }));
    }

    for (const auto& file : files_under({"include/cch/agent", "src/agent"})) {
        if (file.extension() != ".hpp" && file.extension() != ".cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK_FALSE(contains_any(text, {
            "AsyncCliRuntime",
            "coding_agent/runtime",
            "cch/tools/",
            "../tools/",
            "../../tools/",
        }));
    }

    for (const auto& file : files_under({"include/cch/ai/providers", "src/ai/providers"})) {
        if (file.extension() != ".hpp" && file.extension() != ".cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK_FALSE(contains_any(text, {
            "AsyncCliRuntime",
            "cch/tools/",
            "src/tools/",
            "../tools/",
            "../../tools/",
            "coding_agent/runtime",
        }));
    }

    CHECK(std::filesystem::exists(source_root / "CMakeLists.txt"));
}
