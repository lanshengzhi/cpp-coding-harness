#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../../include/cch/coding_agent/Skill.hpp"

using namespace cch;

namespace {

TEST_CASE("Skill aggregate construction and field access", "[coding_agent][skill][u1]") {
    coding_agent::Skill skill{
        .name = "my-skill",
        .description = "Does useful things.",
        .content = "# My Skill\n\nInstructions here.",
        .filePath = "/home/user/.cpp-harness/skills/my-skill/SKILL.md",
    };

    CHECK(skill.name == "my-skill");
    CHECK(skill.description == "Does useful things.");
    CHECK(skill.content == "# My Skill\n\nInstructions here.");
    CHECK(skill.filePath == "/home/user/.cpp-harness/skills/my-skill/SKILL.md");
    CHECK(skill.disableModelInvocation == false);
}

TEST_CASE("Skill with disableModelInvocation", "[coding_agent][skill][u1]") {
    coding_agent::Skill skill{
        .name = "hidden-skill",
        .description = "Only usable via explicit invocation.",
        .content = "",
        .filePath = "/tmp/SKILL.md",
        .disableModelInvocation = true,
    };

    CHECK(skill.disableModelInvocation == true);
}

TEST_CASE("Skill with empty content", "[coding_agent][skill][u1]") {
    coding_agent::Skill skill{
        .name = "frontmatter-only",
        .description = "No body content.",
        .content = "",
        .filePath = "/tmp/SKILL.md",
    };

    CHECK(skill.content.empty());
    CHECK(skill.name == "frontmatter-only");
}

TEST_CASE("SkillDiagnostic construction", "[coding_agent][skill][u1]") {
    coding_agent::SkillDiagnostic diag{
        .type = "warning",
        .code = coding_agent::SkillDiagnosticCode::invalid_metadata,
        .message = "description exceeds 1024 characters",
        .path = "/some/path/SKILL.md",
    };

    CHECK(diag.type == "warning");
    CHECK(diag.code == coding_agent::SkillDiagnosticCode::invalid_metadata);
    CHECK(diag.message == "description exceeds 1024 characters");
    CHECK(diag.path == "/some/path/SKILL.md");
}

TEST_CASE("SkillDiagnostic default values", "[coding_agent][skill][u1]") {
    coding_agent::SkillDiagnostic diag{};

    CHECK(diag.type == "warning");
    CHECK(diag.code == coding_agent::SkillDiagnosticCode::invalid_metadata);
    CHECK(diag.message.empty());
    CHECK(diag.path.empty());
}

TEST_CASE("SkillLoadResult construction with content", "[coding_agent][skill][u1]") {
    coding_agent::SkillLoadResult result{
        .skills = {coding_agent::Skill{
            .name = "a-skill",
            .description = "A.",
            .content = "body",
            .filePath = "/a/SKILL.md",
        }},
        .diagnostics = {coding_agent::SkillDiagnostic{
            .code = coding_agent::SkillDiagnosticCode::duplicate_name,
            .message = "duplicate skill name 'a-skill'",
            .path = "/b/SKILL.md",
        }},
    };

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "a-skill");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::SkillDiagnosticCode::duplicate_name);
}

TEST_CASE("SkillLoadResult empty construction", "[coding_agent][skill][u1]") {
    coding_agent::SkillLoadResult result{};

    CHECK(result.skills.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("SkillDiagnosticCode enum values are distinct", "[coding_agent][skill][u1]") {
    using SCC = coding_agent::SkillDiagnosticCode;
    // Verify no two codes are the same underlying value
    CHECK(static_cast<int>(SCC::file_info_failed) != static_cast<int>(SCC::list_failed));
    CHECK(static_cast<int>(SCC::list_failed) != static_cast<int>(SCC::read_failed));
    CHECK(static_cast<int>(SCC::read_failed) != static_cast<int>(SCC::parse_failed));
    CHECK(static_cast<int>(SCC::parse_failed) != static_cast<int>(SCC::invalid_metadata));
    CHECK(static_cast<int>(SCC::invalid_metadata) != static_cast<int>(SCC::duplicate_name));
}

} // namespace
