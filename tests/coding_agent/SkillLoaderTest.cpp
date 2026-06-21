#include "../../third_party/catch2/catch_test_macros.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "../../src/harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <algorithm>

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

// ── U4: Recursive directory discovery tests ──────────────────────────

TEST_CASE("loadSkills discovers single SKILL.md in root dir", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("SKILL.md",
        "---\n"
        "name: root-skill\n"
        "description: Root level skill.\n"
        "---\n"
        "Body.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    // Only SKILL.md matches, not root .md files (includeRootFiles=false).
    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "root-skill");
}

TEST_CASE("loadSkills discovers nested skill directories", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("skill-a/SKILL.md",
        "---\nname: skill-a\ndescription: First skill.\n---\nBody A.\n");
    fix.writeSkill("skill-b/SKILL.md",
        "---\nname: skill-b\ndescription: Second skill.\n---\nBody B.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 2);
    // Skills should be loaded (order may vary by directory listing sort).
    std::vector<std::string> names;
    for (const auto& s : result.skills) names.push_back(s.name);
    CHECK(std::find(names.begin(), names.end(), "skill-a") != names.end());
    CHECK(std::find(names.begin(), names.end(), "skill-b") != names.end());
}

TEST_CASE("loadSkills with includeRootFiles loads root .md files", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("global-skill.md",
        "---\n"
        "name: global-skill\n"
        "description: A global skill from root .md.\n"
        "---\n"
        "Body.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = true}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "global-skill");
}

TEST_CASE("loadSkills skips dot-prefixed directories", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill(".hidden/skill-hidden/SKILL.md",
        "---\nname: skill-hidden\ndescription: Should be skipped.\n---\nBody.\n");
    fix.writeSkill("visible/SKILL.md",
        "---\nname: visible\ndescription: Should be found.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "visible");
}

TEST_CASE("loadSkills skips node_modules", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("node_modules/some-pkg/SKILL.md",
        "---\nname: pkg-skill\ndescription: Should be skipped.\n---\nBody.\n");
    fix.writeSkill("my-skill/SKILL.md",
        "---\nname: my-skill\ndescription: Should be found.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "my-skill");
}

TEST_CASE("loadSkills deduplicates by name", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("dir-a/SKILL.md",
        "---\nname: same-name\ndescription: First occurrence.\n---\nBody A.\n");
    fix.writeSkill("dir-b/SKILL.md",
        "---\nname: same-name\ndescription: Second occurrence.\n---\nBody B.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "same-name");
    // Should have one duplicate_name diagnostic.
    bool hasDupDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == coding_agent::SkillDiagnosticCode::duplicate_name) hasDupDiag = true;
    }
    CHECK(hasDupDiag);
}

TEST_CASE("loadSkills silently skips missing input directory", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = "nonexistent-dir", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    CHECK(result.skills.empty());
    // Missing dir should not produce diagnostics.
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadSkills handles directory with no SKILL.md", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("empty-dir/placeholder.txt", "not a skill");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    CHECK(result.skills.empty());
}

TEST_CASE("loadSkills continues after malformed skill in one directory", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("bad/SKILL.md",
        "---\nname: bad\ndescription:\n---\nBody.\n");  // missing description → rejected
    fix.writeSkill("good/SKILL.md",
        "---\nname: good\ndescription: Valid skill.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .includeRootFiles = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    // Good skill should still be loaded.
    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "good");
    // Bad skill should produce a diagnostic.
    bool hasDiag = false;
    for (const auto& d : result.diagnostics) {
        if (d.path.find("bad/SKILL.md") != std::string::npos) hasDiag = true;
    }
    CHECK(hasDiag);
}

TEST_CASE("loadSkills loads from multiple input directories", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("dir1/skill1/SKILL.md",
        "---\nname: skill1\ndescription: First.\n---\nBody1.\n");
    fix.writeSkill("dir2/skill2/SKILL.md",
        "---\nname: skill2\ndescription: Second.\n---\nBody2.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {
        {.path = "dir1", .includeRootFiles = false},
        {.path = "dir2", .includeRootFiles = false},
    };
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 2);
}
