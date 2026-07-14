#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/prompt/PromptProcessor.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::Skill test_skill(std::string name, std::string content) {
    return coding_agent::Skill{
        .name = std::move(name),
        .description = "Integration test skill.",
        .content = std::move(content),
        .filePath = "/snapshot/test/SKILL.md",
        .disableModelInvocation = false,
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
    std::vector<coding_agent::Skill> skills = {
        test_skill("int-skill", ""),
    };
    auto block = coding_agent::formatSkillsForPrompt(skills);
    CHECK_FALSE(block.empty());
    CHECK(block.find("int-skill") != std::string::npos);
}

TEST_CASE("prompt processor expands a skill from the immutable snapshot", "[coding_agent][skill-integration][u5]") {
    const auto expanded = process_with_skills(
        "/skill:int-skill",
        {test_skill("int-skill", "# Integration Skill\n\nDo the thing.\n")});

    CHECK(expanded.find("<skill name=\"int-skill\"") != std::string::npos);
    CHECK(expanded.find("Do the thing.") != std::string::npos);
    CHECK(expanded.find("</skill>") != std::string::npos);
}

TEST_CASE("prompt processor appends skill invocation arguments", "[coding_agent][skill-integration][u5]") {
    const auto expanded = process_with_skills(
        "/skill:arg-skill extra args here",
        {test_skill("arg-skill", "Process input.\n")});

    CHECK(expanded.find("</skill>\n\nextra args here") != std::string::npos);
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
