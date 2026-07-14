#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../../include/cch/coding_agent/PromptProcessing.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "coding_agent/SkillFormatting.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <sstream>

using namespace cch;

namespace {

// ── U4: SkillFormatting + steering messages integration ──

TEST_CASE("formatSkillsForPrompt integration with steering", "[coding_agent][skill-integration][u4]") {
    std::vector<coding_agent::Skill> skills = {
        {.name = "int-skill",
         .description = "Integration test skill.",
         .content = "",
         .filePath = "/tmp/test/SKILL.md",
         .disableModelInvocation = false},
    };
    auto block = coding_agent::formatSkillsForPrompt(skills);
    CHECK_FALSE(block.empty());
    CHECK(block.find("int-skill") != std::string::npos);
}

// ── U5: expand_skill_command + process_prompt integration ──

TEST_CASE("expand_skill_command expands valid skill", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace tmp;
    auto skill_dir = tmp.path() / "int-skill";
    tmp.write("int-skill/SKILL.md",
              "---\n"
              "name: int-skill\n"
              "description: Integration test skill.\n"
              "---\n"
              "# Integration Skill\n\n"
              "Do the thing.\n");

    harness::WorkspaceFileSystem fs(tmp.path());
    auto load_result = coding_agent::loadSkillFromFile(
        fs, (skill_dir / "SKILL.md").string());
    REQUIRE(load_result.skills.size() == 1);

    auto expanded = coding_agent::expand_skill_command(
        "/skill:int-skill", load_result.skills, fs);
    CHECK_FALSE(expanded.empty());
    CHECK(expanded != "/skill:int-skill");
    CHECK(expanded.find("<skill name=\"int-skill\"") != std::string::npos);
    CHECK(expanded.find("Do the thing.") != std::string::npos);
    CHECK(expanded.find("</skill>") != std::string::npos);
}

TEST_CASE("expand_skill_command with arguments", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace tmp;
    tmp.write("arg-skill/SKILL.md",
              "---\n"
              "name: arg-skill\n"
              "description: Skill with arg support.\n"
              "---\n"
              "# Arg Skill\n\n"
              "Process input.\n");

    harness::WorkspaceFileSystem fs(tmp.path());
    auto load_result = coding_agent::loadSkillFromFile(
        fs, (tmp.path() / "arg-skill" / "SKILL.md").string());
    REQUIRE(load_result.skills.size() == 1);

    auto expanded = coding_agent::expand_skill_command(
        "/skill:arg-skill extra args here",
        load_result.skills, fs);
    CHECK(expanded.find("</skill>\n\nextra args here") != std::string::npos);
}

TEST_CASE("expand_skill_command unknown skill passes through without printing", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace tmp;
    harness::WorkspaceFileSystem fs(tmp.path());
    std::vector<coding_agent::Skill> skills;

    std::stringstream stderr_capture;
    auto* old_stderr = std::cerr.rdbuf(stderr_capture.rdbuf());
    auto result = coding_agent::expand_skill_command(
        "/skill:unknown-skill", skills, fs);
    std::cerr.rdbuf(old_stderr);

    CHECK(result == "/skill:unknown-skill");
    CHECK(stderr_capture.str().empty());
}

TEST_CASE("expand_skill_command bare prefix passthrough", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace tmp;
    harness::WorkspaceFileSystem fs(tmp.path());
    std::vector<coding_agent::Skill> skills;

    auto result = coding_agent::expand_skill_command("/skill:", skills, fs);
    CHECK(result == "/skill:"); // Passthrough unchanged
}

TEST_CASE("expand_skill_command not a skill command fast path", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace tmp;
    harness::WorkspaceFileSystem fs(tmp.path());
    std::vector<coding_agent::Skill> skills;

    // Not starting with /skill:
    auto result = coding_agent::expand_skill_command("regular text", skills, fs);
    CHECK(result == "regular text");

    // Starts with /skill but not /skill: (e.g., /skills)
    result = coding_agent::expand_skill_command("/skills", skills, fs);
    CHECK(result == "/skills"); // Passthrough — not /skill:
}

TEST_CASE("process_prompt expands skill inline", "[coding_agent][skill-integration][u5]") {
    tests::TempWorkspace tmp;
    tmp.write("proc-skill/SKILL.md",
              "---\n"
              "name: proc-skill\n"
              "description: Process prompt test.\n"
              "---\n"
              "# Proc Skill\n\n"
              "Execute the process.\n");

    harness::WorkspaceFileSystem fs(tmp.path());
    auto load_result = coding_agent::loadSkillFromFile(
        fs, (tmp.path() / "proc-skill" / "SKILL.md").string());
    REQUIRE(load_result.skills.size() == 1);

    coding_agent::CommandRegistry registry;
    std::vector<coding_agent::PromptTemplate> templates;

    auto result = coding_agent::process_prompt(
        "/skill:proc-skill", templates, registry,
        coding_agent::CommandContext{}, load_result.skills, fs);

    CHECK_FALSE(result.command_handled);
    CHECK_FALSE(result.shutdown_requested);
    CHECK_FALSE(result.expanded_prompt.empty());
    CHECK(result.expanded_prompt.find("<skill name=\"proc-skill\"") != std::string::npos);
    CHECK(result.expanded_prompt.find("Execute the process.") != std::string::npos);
}

} // namespace
