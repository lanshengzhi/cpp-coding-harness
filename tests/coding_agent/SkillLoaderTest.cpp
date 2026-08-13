#include "coding_agent/SkillLoader.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

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
    // The body is not preloaded; the file path and base directory are.
    CHECK(result.skills[0].filePath.find("my-skill/SKILL.md") != std::string::npos);
    CHECK(result.skills[0].baseDir.find("my-skill") != std::string::npos);
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

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
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

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
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

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = true}};
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

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
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

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "my-skill");
}

TEST_CASE("loadSkills deduplicates by name with a collision diagnostic", "[coding_agent][skill][u4][issue405]") {
    SkillTestFixture fix;
    fix.writeSkill("dir-a/SKILL.md",
        "---\nname: same-name\ndescription: First occurrence.\n---\nBody A.\n");
    fix.writeSkill("dir-b/SKILL.md",
        "---\nname: same-name\ndescription: Second occurrence.\n---\nBody B.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "same-name");
    // One pi-shaped collision diagnostic with winner/loser paths (other
    // diagnostics are name-validation warnings from the loader).
    const auto collision = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& d) { return d.code == coding_agent::SkillDiagnosticCode::collision; });
    REQUIRE(collision != result.diagnostics.end());
    CHECK(collision->type == "collision");
    CHECK(collision->message == "name \"same-name\" collision");
    REQUIRE(collision->collision.has_value());
    CHECK(collision->collision->resource_type == coding_agent::ResourceCollisionResourceType::Skill);
    CHECK(collision->collision->name == "same-name");
    CHECK(collision->collision->winner_path.find("dir-a/SKILL.md") != std::string::npos);
    CHECK(collision->collision->loser_path.find("dir-b/SKILL.md") != std::string::npos);
}

TEST_CASE("loadSkills silently skips missing input directory", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = "nonexistent-dir", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    CHECK(result.skills.empty());
    // Missing dir should not produce diagnostics.
    CHECK(result.diagnostics.empty());
}

TEST_CASE("loadSkills handles directory with no SKILL.md", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("empty-dir/placeholder.txt", "not a skill");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    CHECK(result.skills.empty());
}

TEST_CASE("loadSkills continues after malformed skill in one directory", "[coding_agent][skill][u4]") {
    SkillTestFixture fix;
    fix.writeSkill("bad/SKILL.md",
        "---\nname: bad\ndescription:\n---\nBody.\n");  // missing description → rejected
    fix.writeSkill("good/SKILL.md",
        "---\nname: good\ndescription: Valid skill.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
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
        {.path = "dir1", .include_root_files = false},
        {.path = "dir2", .include_root_files = false},
    };
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 2);
}

// ── U5: pi discovery provenance, ignore matcher, explicit paths ──────────

TEST_CASE("loadSkills records pi sourceInfo for user/project/path contexts", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    fix.writeSkill("user-dir/skill-a/SKILL.md",
        "---\nname: skill-a\ndescription: User skill.\n---\nBody.\n");
    fix.writeSkill("proj-dir/skill-b/SKILL.md",
        "---\nname: skill-b\ndescription: Project skill.\n---\nBody.\n");
    fix.writeSkill("cli-dir/skill-c/SKILL.md",
        "---\nname: skill-c\ndescription: CLI skill.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {
        {.path = "user-dir",
         .include_root_files = false,
         .source_context = {.source = "auto",
                            .scope = coding_agent::SourceScope::User,
                            .base_dir = "/agent"}},
        {.path = "proj-dir",
         .include_root_files = false,
         .source_context = {.source = "auto",
                            .scope = coding_agent::SourceScope::Project,
                            .base_dir = "/proj/.pi"}},
        {.path = "cli-dir",
         .include_root_files = false,
         .source_context = {.source = "cli",
                            .scope = coding_agent::SourceScope::Temporary,
                            .base_dir = std::nullopt}},
    };
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 3);
    const auto by_name = [&](std::string_view name) -> const coding_agent::Skill& {
        const auto found = std::find_if(
            result.skills.begin(), result.skills.end(),
            [name](const auto& skill) { return skill.name == name; });
        REQUIRE(found != result.skills.end());
        return *found;
    };
    CHECK(by_name("skill-a").sourceInfo.scope == coding_agent::SourceScope::User);
    CHECK(by_name("skill-a").sourceInfo.source == "auto");
    CHECK(by_name("skill-a").sourceInfo.base_dir == "/agent");
    CHECK(by_name("skill-a").sourceInfo.path == by_name("skill-a").filePath);
    CHECK(by_name("skill-a").sourceInfo.origin == coding_agent::SourceOrigin::TopLevel);
    CHECK(by_name("skill-b").sourceInfo.scope == coding_agent::SourceScope::Project);
    CHECK(by_name("skill-b").sourceInfo.base_dir == "/proj/.pi");
    CHECK(by_name("skill-c").sourceInfo.scope == coding_agent::SourceScope::Temporary);
    CHECK(by_name("skill-c").sourceInfo.source == "cli");
    CHECK_FALSE(by_name("skill-c").sourceInfo.base_dir.has_value());
    // baseDir is the skill's own directory, not the resource root.
    CHECK(by_name("skill-a").baseDir.find("user-dir/skill-a") != std::string::npos);
}

TEST_CASE("loadSkills applies a root .gitignore with pi prefix and negation semantics", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    // A root-level pattern applies at any depth (basename match); the
    // negated pattern re-includes the specific file. Git/pi semantics: a
    // pruned directory is never re-entered, so file-level negation is what
    // re-includes.
    fix.workspace.write(".gitignore", "root-skill.md\n!keep.md\nignored-dir/\n");
    fix.writeSkill("ignored-dir/drop/SKILL.md",
        "---\nname: drop\ndescription: Ignored directory.\n---\nBody.\n");
    fix.writeSkill("root-skill.md",
        "---\nname: root-skill\ndescription: Ignored root md.\n---\nBody.\n");
    fix.writeSkill("keep.md",
        "---\nname: keep\ndescription: Negated back in.\n---\nBody.\n");
    fix.writeSkill("sub/SKILL.md",
        "---\nname: visible\ndescription: Unaffected.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = true}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    std::vector<std::string> names;
    for (const auto& s : result.skills) names.push_back(s.name);
    CHECK(std::find(names.begin(), names.end(), "keep") != names.end());
    CHECK(std::find(names.begin(), names.end(), "root-skill") == names.end());
    CHECK(std::find(names.begin(), names.end(), "drop") == names.end());
    CHECK(std::find(names.begin(), names.end(), "visible") != names.end());
}

TEST_CASE("loadSkills prefixes subdirectory ignore rules", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    // nested/.gitignore patterns are prefixed with `nested/`, so they apply
    // beneath the nested directory only — the sibling directory is
    // unaffected by a rule that would match it by name.
    fix.writeSkill("nested/.gitignore", "sub/\n");
    fix.writeSkill("nested/sub/SKILL.md",
        "---\nname: nested-skill\ndescription: Pruned by nested/.gitignore.\n---\nBody.\n");
    fix.writeSkill("sub/SKILL.md",
        "---\nname: sibling-skill\ndescription: Sibling kept.\n---\nBody.\n");
    fix.writeSkill("root-skill.md",
        "---\nname: root-skill\ndescription: Root level.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = true}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    std::vector<std::string> names;
    for (const auto& s : result.skills) names.push_back(s.name);
    CHECK(std::find(names.begin(), names.end(), "root-skill") != names.end());
    CHECK(std::find(names.begin(), names.end(), "sibling-skill") != names.end());
    CHECK(std::find(names.begin(), names.end(), "nested-skill") == names.end());
}

TEST_CASE("loadSkills reads .ignore and .fdignore alongside .gitignore", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    fix.workspace.write(".ignore", "from-ignore/\n");
    fix.workspace.write(".fdignore", "from-fd/\n");
    fix.writeSkill("from-ignore/s/SKILL.md",
        "---\nname: from-ignore\ndescription: .ignore hit.\n---\nBody.\n");
    fix.writeSkill("from-fd/s/SKILL.md",
        "---\nname: from-fd\ndescription: .fdignore hit.\n---\nBody.\n");
    fix.writeSkill("kept/SKILL.md",
        "---\nname: kept\ndescription: Kept.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "kept");
}

TEST_CASE("loadSkills skips comments and blank ignore lines and honors escapes", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    // The comment and blank lines carry no rule; the escaped `\#` pattern is
    // a literal `#escaped-comment/` directory rule (pi `prefixIgnorePattern`).
    fix.workspace.write(".gitignore", "# a comment\n\n\\#escaped-comment/\n");
    fix.writeSkill("#escaped-comment/s/SKILL.md",
        "---\nname: escaped-comment\ndescription: Escaped hash dir.\n---\nBody.\n");
    fix.writeSkill("kept/SKILL.md",
        "---\nname: kept\ndescription: Kept.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "kept");
}

TEST_CASE("loadSkills ignores a SKILL.md file directly", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    fix.workspace.write(".gitignore", "hidden-skill/SKILL.md\n");
    fix.writeSkill("hidden-skill/SKILL.md",
        "---\nname: hidden-skill\ndescription: Ignored file.\n---\nBody.\n");
    fix.writeSkill("visible/SKILL.md",
        "---\nname: visible\ndescription: Not ignored.\n---\nBody.\n");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "visible");
}

TEST_CASE("loadSkills loads explicit file paths and warns on non-markdown files", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    fix.writeSkill("explicit/SKILL.md",
        "---\nname: explicit-skill\ndescription: Explicit.\n---\nBody.\n");
    fix.workspace.write("notes.txt", "not a skill");

    std::vector<coding_agent::SkillDirSpec> dirs = {
        {.path = "explicit/SKILL.md",
         .include_root_files = true,
         .source_context = {.source = "cli",
                            .scope = coding_agent::SourceScope::Temporary,
                            .base_dir = std::nullopt}},
        {.path = "notes.txt",
         .include_root_files = true,
         .source_context = {.source = "cli",
                            .scope = coding_agent::SourceScope::Temporary,
                            .base_dir = std::nullopt}},
    };
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "explicit-skill");
    CHECK(result.skills[0].sourceInfo.source == "cli");
    bool warned = false;
    for (const auto& d : result.diagnostics) {
        if (d.message == "skill path is not a markdown file") warned = true;
    }
    CHECK(warned);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("loadSkills deduplicates the same real file reached via symlink", "[coding_agent][skill][u5][issue412]") {
    SkillTestFixture fix;
    fix.writeSkill("real/SKILL.md",
        "---\nname: real-skill\ndescription: The real file.\n---\nBody.\n");
    std::filesystem::create_directories(fix.workspace.path() / "alias");
    std::filesystem::create_directory_symlink(
        fix.workspace.path() / "real", fix.workspace.path() / "alias" / "real");

    std::vector<coding_agent::SkillDirSpec> dirs = {{.path = ".", .include_root_files = false}};
    auto result = coding_agent::loadSkills(fix.fs, dirs);

    // Only one skill despite two paths to the same real file; no collision
    // diagnostic.
    REQUIRE(result.skills.size() == 1);
    CHECK(result.skills[0].name == "real-skill");
    bool collision = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == coding_agent::SkillDiagnosticCode::collision) collision = true;
    }
    CHECK_FALSE(collision);
}
#endif
