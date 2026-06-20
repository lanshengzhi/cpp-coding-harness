#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../../include/cch/coding_agent/SkillLoader.hpp"
#include "../../src/harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

using namespace cch;

namespace {

/// Helper: create a WorkspaceFileSystem from a TempWorkspace and write a SKILL.md.
struct SkillTestFixture {
    tests::TempWorkspace workspace;
    harness::WorkspaceFileSystem fs;

    SkillTestFixture() : fs(*harness::WorkspaceFileSystem::create(workspace.path())) {}

    void writeSkill(const std::string& relativePath, const std::string& content) {
        workspace.write(relativePath, content);
    }

    std::string skillPath(const std::string& relativePath) const {
        return (workspace.path() / relativePath).string();
    }

    coding_agent::SkillLoadResult load(const std::string& relativePath) {
        // WorkspaceFileSystem::readTextFile requires workspace-relative paths.
        return coding_agent::loadSkillFromFile(fs, relativePath);
    }
};

} // namespace

TEST_CASE("loadSkillFromFile loads valid SKILL.md", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("my-skill/SKILL.md",
        "---\n"
        "name: my-skill\n"
        "description: Does useful things.\n"
        "---\n"
        "# My Skill\n\nInstructions here.\n");

    auto result = fix.load("my-skill/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "my-skill");
    CHECK(result.skills[0].description == "Does useful things.");
    CHECK(result.skills[0].content == "# My Skill\n\nInstructions here.");
    CHECK(result.skills[0].disableModelInvocation == false);
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadSkillFromFile with disable-model-invocation", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("hidden-skill/SKILL.md",
        "---\n"
        "name: hidden-skill\n"
        "description: Hidden skill.\n"
        "disable-model-invocation: true\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("hidden-skill/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].disableModelInvocation == true);
}

TEST_CASE("loadSkillFromFile derives name from parent dir when frontmatter omits name", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("derived-name/SKILL.md",
        "---\n"
        "description: No name in frontmatter.\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("derived-name/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "derived-name");
}

TEST_CASE("loadSkillFromFile warns on description > 1024 chars but still loads", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    std::string longDesc(1025, 'x');
    fix.writeSkill("long-desc/SKILL.md",
        "---\n"
        "name: long-desc\n"
        "description: " + longDesc + "\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("long-desc/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::SkillDiagnosticCode::invalid_metadata);
}

TEST_CASE("loadSkillFromFile warns on uppercase in name", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("my-skill/SKILL.md",
        "---\n"
        "name: My-Skill\n"
        "description: Has uppercase.\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("my-skill/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    REQUIRE(result.diagnostics.size() >= 1);
    bool hasCharDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("invalid characters") != std::string::npos) hasCharDiag = true;
    }
    CHECK(hasCharDiag);
}

TEST_CASE("loadSkillFromFile warns on leading hyphen in name", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("-bad-name/SKILL.md",
        "---\n"
        "name: -bad-name\n"
        "description: Leading hyphen.\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("-bad-name/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    REQUIRE(result.diagnostics.size() >= 1);
    bool hasDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("must not start with a hyphen") != std::string::npos) hasDiag = true;
    }
    CHECK(hasDiag);
}

TEST_CASE("loadSkillFromFile warns on consecutive hyphens in name", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("bad--name/SKILL.md",
        "---\n"
        "name: bad--name\n"
        "description: Consecutive hyphens.\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("bad--name/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    REQUIRE(result.diagnostics.size() >= 1);
    bool hasDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("consecutive hyphens") != std::string::npos) hasDiag = true;
    }
    CHECK(hasDiag);
}

TEST_CASE("loadSkillFromFile warns on name/directory mismatch", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("actual-dir/SKILL.md",
        "---\n"
        "name: different-name\n"
        "description: Mismatched name.\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("actual-dir/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    REQUIRE(result.diagnostics.size() >= 1);
    bool hasDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("does not match parent directory") != std::string::npos) hasDiag = true;
    }
    CHECK(hasDiag);
}

TEST_CASE("loadSkillFromFile tolerates unknown frontmatter keys", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("extra-keys/SKILL.md",
        "---\n"
        "name: extra-keys\n"
        "description: Has extras.\n"
        "license: MIT\n"
        "metadata: {}\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("extra-keys/SKILL.md");

    REQUIRE(result.skills.size() == 1);
    // Unknown keys should not produce diagnostics.
    for (const auto& d : result.diagnostics) {
        CHECK(d.message.find("license") == std::string::npos);
        CHECK(d.message.find("metadata") == std::string::npos);
    }
}

TEST_CASE("loadSkillFromFile rejects skill with missing description", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("no-desc/SKILL.md",
        "---\n"
        "name: no-desc\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("no-desc/SKILL.md");

    CHECK(result.skills.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::SkillDiagnosticCode::invalid_metadata);
    CHECK(result.diagnostics[0].message == "description is required");
}

TEST_CASE("loadSkillFromFile rejects skill with empty description", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("empty-desc/SKILL.md",
        "---\n"
        "name: empty-desc\n"
        "description:\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("empty-desc/SKILL.md");

    CHECK(result.skills.empty());
    REQUIRE(result.diagnostics.size() >= 1);
    bool hasDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.message == "description is required") hasDiag = true;
    }
    CHECK(hasDiag);
}

TEST_CASE("loadSkillFromFile rejects skill with whitespace-only description", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("ws-desc/SKILL.md",
        "---\n"
        "name: ws-desc\n"
        "description: \"   \"\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("ws-desc/SKILL.md");

    // Whitespace-only should be stripped to empty by parser → rejected.
    CHECK(result.skills.empty());
    REQUIRE(result.diagnostics.size() >= 1);
}

TEST_CASE("loadSkillFromFile handles file read failure", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    // Don't create the file.

    auto result = fix.load("nonexistent/SKILL.md");

    CHECK(result.skills.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::SkillDiagnosticCode::read_failed);
}

TEST_CASE("loadSkillFromFile handles YAML parse failure", "[coding_agent][skill][u3]") {
    SkillTestFixture fix;
    fix.writeSkill("bad-yaml/SKILL.md",
        "---\n"
        "name: bad-yaml\n"
        "this line has no colon\n"
        "description: Should fail.\n"
        "---\n"
        "Body.\n");

    auto result = fix.load("bad-yaml/SKILL.md");

    CHECK(result.skills.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].code == coding_agent::SkillDiagnosticCode::parse_failed);
}
