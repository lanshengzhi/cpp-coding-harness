#include "../../include/cch/coding_agent/Skill.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

namespace {

TEST_CASE("Skill aggregate construction and field access", "[coding_agent][skill][u1]") {
    coding_agent::Skill skill{
        .name = "my-skill",
        .description = "Does useful things.",
        .filePath = "/home/user/.pi/agent/skills/my-skill/SKILL.md",
        .baseDir = "/home/user/.pi/agent/skills/my-skill",
        .sourceInfo = coding_agent::SourceInfo{
            .path = "/home/user/.pi/agent/skills/my-skill/SKILL.md",
            .source = "auto",
            .scope = coding_agent::SourceScope::User,
            .origin = coding_agent::SourceOrigin::TopLevel,
            .base_dir = "/home/user/.pi/agent",
        },
    };

    CHECK(skill.name == "my-skill");
    CHECK(skill.description == "Does useful things.");
    CHECK(skill.filePath == "/home/user/.pi/agent/skills/my-skill/SKILL.md");
    CHECK(skill.baseDir == "/home/user/.pi/agent/skills/my-skill");
    CHECK(skill.sourceInfo.scope == coding_agent::SourceScope::User);
    CHECK(skill.sourceInfo.source == "auto");
    CHECK(skill.sourceInfo.base_dir == "/home/user/.pi/agent");
    CHECK(skill.disableModelInvocation == false);
}

TEST_CASE("Skill with disableModelInvocation", "[coding_agent][skill][u1]") {
    coding_agent::Skill skill{
        .name = "hidden-skill",
        .description = "Only usable via explicit invocation.",
        .filePath = "/tmp/SKILL.md",
        .baseDir = "/tmp",
        .sourceInfo = coding_agent::SourceInfo{
            .path = "/tmp/SKILL.md",
            .source = "cli",
            .scope = coding_agent::SourceScope::Temporary,
            .origin = coding_agent::SourceOrigin::TopLevel,
            .base_dir = std::nullopt,
        },
        .disableModelInvocation = true,
    };

    CHECK(skill.disableModelInvocation == true);
    CHECK(skill.sourceInfo.source == "cli");
    CHECK_FALSE(skill.sourceInfo.base_dir.has_value());
}

TEST_CASE("Skill carries no preloaded content", "[coding_agent][skill][u1]") {
    coding_agent::Skill skill{
        .name = "frontmatter-only",
        .description = "No body content.",
        .filePath = "/tmp/SKILL.md",
        .baseDir = "/tmp",
        .sourceInfo = coding_agent::SourceInfo{
            .path = "/tmp/SKILL.md",
            .source = "auto",
            .scope = coding_agent::SourceScope::Project,
            .origin = coding_agent::SourceOrigin::TopLevel,
            .base_dir = "/project/.pi",
        },
    };

    // The body is read at invocation time; the value carries provenance only.
    CHECK(skill.name == "frontmatter-only");
    CHECK(skill.sourceInfo.scope == coding_agent::SourceScope::Project);
    CHECK(skill.sourceInfo.base_dir == "/project/.pi");
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

TEST_CASE("SkillLoadResult construction with provenance", "[coding_agent][skill][u1]") {
    coding_agent::SkillLoadResult result{
        .skills = {coding_agent::Skill{
            .name = "a-skill",
            .description = "A.",
            .filePath = "/a/SKILL.md",
            .baseDir = "/a",
            .sourceInfo = coding_agent::SourceInfo{
                .path = "/a/SKILL.md",
                .source = "auto",
                .scope = coding_agent::SourceScope::Temporary,
                .origin = coding_agent::SourceOrigin::TopLevel,
                .base_dir = std::nullopt,
            },
        }},
        .diagnostics = {coding_agent::SkillDiagnostic{
            .type = "collision",
            .code = coding_agent::SkillDiagnosticCode::collision,
            .message = "name \"a-skill\" collision",
            .path = "/b/SKILL.md",
            .collision = coding_agent::ResourceCollision{
                .resource_type = coding_agent::ResourceCollisionResourceType::Skill,
                .name = "a-skill",
                .winner_path = "/a/SKILL.md",
                .loser_path = "/b/SKILL.md",
            },
        }},
    };

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "a-skill");
    CHECK(result.skills[0].baseDir == "/a");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::SkillDiagnosticCode::collision);
    CHECK(result.diagnostics[0].collision.has_value());
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
    CHECK(static_cast<int>(SCC::invalid_metadata) != static_cast<int>(SCC::collision));
}

} // namespace
