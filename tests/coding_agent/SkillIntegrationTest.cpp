#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/prompt/PromptProcessor.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::Skill test_skill_on_disk(
    tests::TempWorkspace& workspace,
    std::string name,
    std::string body) {
    const auto relative = "skills/" + name + "/SKILL.md";
    workspace.write(
        relative,
        "---\n"
        "name: " + name + "\n"
        "description: Integration test skill.\n"
        "---\n" + body);
    const auto file_path = (workspace.path() / relative).string();
    return coding_agent::Skill{
        .name = std::move(name),
        .description = "Integration test skill.",
        .filePath = file_path,
        .baseDir = std::filesystem::path{file_path}.parent_path().string(),
        .sourceInfo = coding_agent::SourceInfo{
            .path = file_path,
            .source = "auto",
            .scope = coding_agent::SourceScope::Temporary,
            .origin = coding_agent::SourceOrigin::TopLevel,
            .base_dir = std::nullopt,
        },
    };
}

[[nodiscard]] std::string process_with_skills(
    std::string input,
    std::vector<coding_agent::Skill> skills) {
    coding_agent::prompt::PromptProcessor processor{
        std::move(skills), std::vector<coding_agent::PromptTemplate>{}};
    return processor.process(std::move(input), true).text;
}

} // namespace

TEST_CASE("formatSkillsForPrompt integration with steering", "[coding_agent][skill-integration][u4]") {
    tests::TempWorkspace workspace;
    std::vector<coding_agent::Skill> skills = {
        test_skill_on_disk(workspace, "int-skill", ""),
    };
    auto block = coding_agent::formatSkillsForPrompt(skills);
    CHECK_FALSE(block.empty());
    CHECK(block.find("int-skill") != std::string::npos);
}

TEST_CASE("prompt processor expands a skill by reading its file at invocation", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace workspace;
    const auto expanded = process_with_skills(
        "/skill:int-skill",
        {test_skill_on_disk(workspace, "int-skill", "# Integration Skill\n\nDo the thing.\n")});

    CHECK(expanded.find("<skill name=\"int-skill\"") != std::string::npos);
    CHECK(expanded.find("Do the thing.") != std::string::npos);
    CHECK(expanded.find("</skill>") != std::string::npos);
}

TEST_CASE("prompt processor appends skill invocation arguments", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace workspace;
    const auto expanded = process_with_skills(
        "/skill:arg-skill extra args here",
        {test_skill_on_disk(workspace, "arg-skill", "Process input.\n")});

    CHECK(expanded.find("</skill>\n\nextra args here") != std::string::npos);
}

TEST_CASE("prompt processor trims skill invocation arguments like pi", "[coding_agent][skill-integration][u5][issue412]") {
    tests::TempWorkspace workspace;
    // pi `_expandSkillCommand`: `args = text.slice(spaceIndex + 1).trim()`.
    const auto expanded = process_with_skills(
        "/skill:trim-skill   padded args here  \t \n",
        {test_skill_on_disk(workspace, "trim-skill", "Trim.\n")});

    CHECK(expanded.find("</skill>\n\npadded args here") != std::string::npos);
    CHECK(expanded.find("padded args here \t") == std::string::npos);
}

TEST_CASE("prompt processor passes through unknown skill input without printing", "[coding_agent][skill-integration][u5]") {
    std::stringstream stderr_capture;
    auto* old_stderr = std::cerr.rdbuf(stderr_capture.rdbuf());
    const auto result = process_with_skills("/skill:unknown-skill", {});
    std::cerr.rdbuf(old_stderr);

    CHECK(result == "/skill:unknown-skill");
    CHECK(stderr_capture.str().empty());
}

TEST_CASE("prompt processor passes through malformed and non-skill input", "[coding_agent][skill-integration][u5]") {
    CHECK(process_with_skills("/skill:", {}) == "/skill:");
    CHECK(process_with_skills("regular text", {}) == "regular text");
    CHECK(process_with_skills("/skills", {}) == "/skills");
}
