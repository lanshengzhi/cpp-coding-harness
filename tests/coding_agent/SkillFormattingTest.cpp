#include "coding_agent/SkillFormatting.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

namespace {

// ── U1: formatSkillsForPrompt ──

TEST_CASE("formatSkillsForPrompt empty skills returns empty", "[coding_agent][skill-formatting][u1]") {
    std::vector<coding_agent::Skill> skills;
    auto result = coding_agent::formatSkillsForPrompt(skills);
    CHECK(result.empty());
}

TEST_CASE("formatSkillsForPrompt all disabled returns empty", "[coding_agent][skill-formatting][u1]") {
    std::vector<coding_agent::Skill> skills = {
        {.name = "test-skill",
         .description = "Does something.",
         .filePath = "/path/to/skill.md",
         .disableModelInvocation = true},
    };
    auto result = coding_agent::formatSkillsForPrompt(skills);
    CHECK(result.empty());
}

TEST_CASE("formatSkillsForPrompt one visible skill", "[coding_agent][skill-formatting][u1]") {
    std::vector<coding_agent::Skill> skills = {
        {.name = "my-skill",
         .description = "Does things.",
         .filePath = "/home/user/skills/my-skill/SKILL.md",
         .disableModelInvocation = false},
    };
    auto result = coding_agent::formatSkillsForPrompt(skills);
    CHECK_FALSE(result.empty());
    // Contains prose intro
    CHECK(result.find("The following skills provide") != std::string::npos);
    // Contains <available_skills> wrapper
    CHECK(result.find("<available_skills>") != std::string::npos);
    CHECK(result.find("</available_skills>") != std::string::npos);
    // Contains skill entry
    CHECK(result.find("<skill>") != std::string::npos);
    CHECK(result.find("</skill>") != std::string::npos);
    // Contains name, description, location
    CHECK(result.find("<name>my-skill</name>") != std::string::npos);
    CHECK(result.find("<description>Does things.</description>") != std::string::npos);
    CHECK(result.find("<location>/home/user/skills/my-skill/SKILL.md</location>")
          != std::string::npos);
}

TEST_CASE("formatSkillsForPrompt mixed visible and disabled", "[coding_agent][skill-formatting][u1]") {
    std::vector<coding_agent::Skill> skills = {
        {.name = "visible-skill",
         .description = "Visible.",
         .filePath = "/path/a/SKILL.md",
         .disableModelInvocation = false},
        {.name = "hidden-skill",
         .description = "Hidden.",
         .filePath = "/path/b/SKILL.md",
         .disableModelInvocation = true},
        {.name = "another-visible",
         .description = "Also visible.",
         .filePath = "/path/c/SKILL.md",
         .disableModelInvocation = false},
    };
    auto result = coding_agent::formatSkillsForPrompt(skills);
    // Visible skills are present
    CHECK(result.find("visible-skill") != std::string::npos);
    CHECK(result.find("another-visible") != std::string::npos);
    // Hidden skill is absent
    CHECK(result.find("hidden-skill") == std::string::npos);
}

TEST_CASE("formatSkillsForPrompt xml escaping", "[coding_agent][skill-formatting][u1]") {
    std::vector<coding_agent::Skill> skills = {
        {.name = "test&skill",
         .description = "Does <things> & \"stuff\" with 'quotes'.",
         .filePath = "/path/with&ampersand/SKILL.md",
         .disableModelInvocation = false},
    };
    auto result = coding_agent::formatSkillsForPrompt(skills);
    // & → &amp;
    CHECK(result.find("test&amp;skill") != std::string::npos);
    CHECK(result.find("test&skill") == std::string::npos);
    // < → &lt;
    CHECK(result.find("&lt;things&gt;") != std::string::npos);
    // " → &quot;
    CHECK(result.find("&quot;stuff&quot;") != std::string::npos);
    // ' → &apos;
    CHECK(result.find("&apos;quotes&apos;") != std::string::npos);
    // filePath with &
    CHECK(result.find("with&amp;ampersand") != std::string::npos);
}

TEST_CASE("formatSkillsForPrompt multiline description", "[coding_agent][skill-formatting][u1]") {
    std::vector<coding_agent::Skill> skills = {
        {.name = "multi-line",
         .description = "Line one.\nLine two.\nLine three.",
         .filePath = "/path/SKILL.md",
         .disableModelInvocation = false},
    };
    auto result = coding_agent::formatSkillsForPrompt(skills);
    // Newlines preserved verbatim inside <description>
    CHECK(result.find("Line one.\nLine two.\nLine three.") != std::string::npos);
}

TEST_CASE("formatSkillsForPrompt long description included", "[coding_agent][skill-formatting][u1]") {
    // Description > 1024 chars is a validation warning, not a rejection
    std::string long_desc(1050, 'x');
    std::vector<coding_agent::Skill> skills = {
        {.name = "long-desc",
         .description = long_desc,
         .filePath = "/path/SKILL.md",
         .disableModelInvocation = false},
    };
    auto result = coding_agent::formatSkillsForPrompt(skills);
    CHECK(result.find(long_desc) != std::string::npos);
}

// ── U2: formatSkillInvocation ──

TEST_CASE("formatSkillInvocation basic", "[coding_agent][skill-formatting][u2]") {
    coding_agent::Skill skill{.name = "my-skill",
                              .description = "Does things.",
                              .filePath = "/home/user/skills/my-skill/SKILL.md",
                              .baseDir = "/home/user/skills/my-skill"};
    auto result = coding_agent::formatSkillInvocation(skill, "The body content.");
    CHECK(result.find("<skill name=\"my-skill\"") != std::string::npos);
    CHECK(result.find("location=\"/home/user/skills/my-skill/SKILL.md\"") != std::string::npos);
    CHECK(result.find("References are relative to /home/user/skills/my-skill")
          != std::string::npos);
    CHECK(result.find("The body content.") != std::string::npos);
    CHECK(result.find("</skill>") != std::string::npos);
}

TEST_CASE("formatSkillInvocation with additional instructions", "[coding_agent][skill-formatting][u2]") {
    coding_agent::Skill skill{.name = "my-skill",
                              .description = "Does things.",
                              .filePath = "/home/user/skills/my-skill/SKILL.md",
                              .baseDir = "/home/user/skills/my-skill"};
    auto result = coding_agent::formatSkillInvocation(skill, "The body content.", "extra args here");
    CHECK(result.find("</skill>\n\nextra args here") != std::string::npos);
}

TEST_CASE("formatSkillInvocation empty content", "[coding_agent][skill-formatting][u2]") {
    coding_agent::Skill skill{.name = "empty-body", .filePath = "/root/skill.md"};
    auto result = coding_agent::formatSkillInvocation(skill, "");
    // Content area exists between tags, even if empty
    CHECK(result.find("\n\n</skill>") != std::string::npos);
}

TEST_CASE("formatSkillInvocation content with xml special chars not escaped", "[coding_agent][skill-formatting][u2]") {
    // Content inside <skill> is raw body — NOT XML-escaped
    coding_agent::Skill skill{.name = "raw-body", .filePath = "/path/SKILL.md"};
    auto result = coding_agent::formatSkillInvocation(skill, "<tag>&amp;content&quot;");
    CHECK(result.find("<tag>&amp;content&quot;") != std::string::npos);
}

TEST_CASE("formatSkillInvocation root directory", "[coding_agent][skill-formatting][u2]") {
    coding_agent::Skill skill{.name = "root-skill", .filePath = "/SKILL.md", .baseDir = "/"};
    auto result = coding_agent::formatSkillInvocation(skill, "Root content.");
    CHECK(result.find("References are relative to /") != std::string::npos);
}

TEST_CASE("formatSkillInvocation additional instructions whitespace", "[coding_agent][skill-formatting][u2]") {
    coding_agent::Skill skill{.name = "ws-skill", .filePath = "/path/SKILL.md"};
    auto result = coding_agent::formatSkillInvocation(skill, "body", "  spaced  ");
    // Whitespace preserved verbatim
    CHECK(result.find("\n\n  spaced  ") != std::string::npos);
}

} // namespace
