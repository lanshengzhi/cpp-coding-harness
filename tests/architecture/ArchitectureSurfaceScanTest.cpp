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

std::vector<std::filesystem::path> public_headers() {
    std::vector<std::filesystem::path> headers;
    for (const auto& path : files_under({"include/cch"})) {
        if (path.extension() == ".hpp") {
            headers.push_back(path);
        }
    }
    return headers;
}

} // namespace

TEST_CASE("library publishes include as contract surface and keeps src private", "[architecture][u1]") {
    const auto cmake = read_text(std::filesystem::path(CCH_SOURCE_DIR) / "CMakeLists.txt");

    CHECK(cmake.find("${CMAKE_CURRENT_SOURCE_DIR}/include") != std::string::npos);
    CHECK(cmake.find("PRIVATE\n        ${CMAKE_CURRENT_SOURCE_DIR}/src") != std::string::npos);
    CHECK(cmake.find("PUBLIC\n        ${CMAKE_CURRENT_SOURCE_DIR}/include\n        ${CMAKE_CURRENT_SOURCE_DIR}/src") == std::string::npos);
}

TEST_CASE("public headers do not include private src paths", "[architecture][u1]") {
    const auto headers = public_headers();
    REQUIRE_FALSE(headers.empty());

    for (const auto& header : headers) {
        const auto text = read_text(header);
        CHECK(text.find("#include \"../../src") == std::string::npos);
        CHECK(text.find("#include <src/") == std::string::npos);
    }
}

TEST_CASE("core public contracts do not expose Glaze generic machinery", "[architecture][u7]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);

    const auto error_header = read_text(source_root / "include" / "cch" / "util" / "Error.hpp");
    CHECK(error_header.find("glaze/glaze.hpp") == std::string::npos);
    CHECK(error_header.find("glz::") == std::string::npos);
    CHECK(error_header.find("read_json") == std::string::npos);
    CHECK(error_header.find("write_json") == std::string::npos);

    const auto domain_headers = {
        source_root / "include" / "cch" / "ai" / "Content.hpp",
        source_root / "include" / "cch" / "ai" / "Message.hpp",
        source_root / "include" / "cch" / "agent" / "AgentTool.hpp",
    };
    for (const auto& header : domain_headers) {
        const auto text = read_text(header);
        CHECK(text.find("glaze/glaze.hpp") == std::string::npos);
        CHECK(text.find("glz::generic") == std::string::npos);
        CHECK(text.find("JsonValue") != std::string::npos);
    }
}

TEST_CASE("provider DTOs stay out of the public contract surface", "[architecture][u4]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "ai" / "glaze" / "ProviderDtos.hpp"));
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "util" / "Json.hpp"));

    const auto glaze_dir = source_root / "include" / "cch" / "ai" / "glaze";
    if (std::filesystem::exists(glaze_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(glaze_dir)) {
            CHECK(entry.path().extension() != ".hpp");
        }
    }

    const auto openai_header = read_text(source_root / "include" / "cch" / "ai" / "providers" / "OpenAIChatClient.hpp");
    CHECK(openai_header.find("BoostBeastStreamTransport.hpp") == std::string::npos);
    CHECK(openai_header.find("StreamTransport.hpp") != std::string::npos);

    const auto providers_dir = source_root / "include" / "cch" / "ai" / "providers";
    const std::vector<std::string> allowed_provider_headers = {
        "StreamTransport.hpp",
        "OpenAIChatClient.hpp",
        "OpenAICompletionsCompat.hpp",
    };
    for (const auto& entry : std::filesystem::directory_iterator(providers_dir)) {
        if (entry.path().extension() != ".hpp") {
            continue;
        }
        const auto filename = entry.path().filename().string();
        CHECK(std::find(allowed_provider_headers.begin(), allowed_provider_headers.end(), filename) != allowed_provider_headers.end());
    }
}

TEST_CASE("coding_agent loaders stay out of the public contract surface", "[architecture][u4]") {
    const auto source_root = std::filesystem::path(CCH_SOURCE_DIR);
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "coding_agent" / "SkillLoader.hpp"));
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "coding_agent" / "PromptTemplateLoader.hpp"));
    CHECK_FALSE(std::filesystem::exists(source_root / "include" / "cch" / "coding_agent" / "SkillFormatting.hpp"));
}

TEST_CASE("active source tree does not retain legacy sync contracts", "[architecture][u2]") {
    const auto files = files_under({"include", "src", "tests"});
    REQUIRE_FALSE(files.empty());

    for (const auto& file : files) {
        if (file.filename() == "ArchitectureSurfaceScanTest.cpp") {
            continue;
        }
        const auto text = read_text(file);
        CHECK(text.find("util::Result") == std::string::npos);
        CHECK(text.find("boost::json") == std::string::npos);
        CHECK(text.find("src/util/Result.hpp") == std::string::npos);
        CHECK(text.find("src/tools/Tools.hpp") == std::string::npos);
        CHECK(text.find("make_read_file_tool") == std::string::npos);
        CHECK(text.find("make_bash_tool") == std::string::npos);
    }
}
