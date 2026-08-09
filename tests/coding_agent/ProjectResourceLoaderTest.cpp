#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/ProjectResourceLoader.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

using namespace cch;

namespace {

struct LoaderFixture {
    tests::TempWorkspace workspace;
    harness::WorkspaceFileSystem fs;
    coding_agent::ProjectTrustStore trust_store;

    LoaderFixture()
        : fs(*harness::WorkspaceFileSystem::create(workspace.path())),
          trust_store(workspace.path() / "trust.json") {}

    void write(std::string relative, std::string content) const {
        workspace.write(std::move(relative), std::move(content));
    }

    coding_agent::ProjectResourceLoadingResult load(
        coding_agent::ProjectResourceLoadingRequest request = {}) const {
        request.workspace = workspace.path();
        return coding_agent::load_project_resources(fs, trust_store, std::move(request));
    }
};

void write_valid_project_skill(const LoaderFixture& fix, std::string name = "project-skill") {
    fix.write(
        ".pi/skills/" + name + "/SKILL.md",
        "---\n"
        "name: " + name + "\n"
        "description: Project skill.\n"
        "---\n"
        "Project skill body.\n");
}

void write_valid_project_prompt(const LoaderFixture& fix, std::string name = "review") {
    fix.write(
        ".pi/prompts/" + name + ".md",
        "---\n"
        "description: Project prompt.\n"
        "---\n"
        "Project prompt body.\n");
}

[[nodiscard]] std::string valid_theme_json() {
    std::ifstream input(std::filesystem::path(CCH_SOURCE_DIR) / "tests" / "fixtures" / "themes" / "dark.json");
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

bool has_diag(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ResourceDiagnosticType type,
    std::string_view message_fragment = {}) {
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [type, message_fragment](const auto& diagnostic) {
            return diagnostic.type == type &&
                   (message_fragment.empty() ||
                    diagnostic.message.find(message_fragment) != std::string::npos);
        });
}

std::size_t count_diag(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ResourceDiagnosticType type) {
    return static_cast<std::size_t>(std::count_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [type](const auto& diagnostic) { return diagnostic.type == type; }));
}

} // namespace

TEST_CASE("project resource loader loads trusted .pi/ skills and prompts", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    write_valid_project_prompt(fix);

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].name == "project-skill");
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "review");
    CHECK(result.resources.prompt_templates[0].filePath.find(".pi/prompts/review.md") != std::string::npos);
    CHECK(result.diagnostics.empty());
}

TEST_CASE(
    "project resource loader returns raw .pi/ themes only when the TUI requests trusted resources",
    "[coding_agent][project-resource-loader][theme][issue56][issue405]") {
    LoaderFixture fix;
    fix.write(".pi/themes/project.json", valid_theme_json());

    coding_agent::ProjectResourceLoadingRequest default_request;
    default_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    const auto disabled = fix.load(std::move(default_request));
    CHECK(disabled.resources.project_themes.empty());

    coding_agent::ProjectResourceLoadingRequest untrusted_request;
    untrusted_request.theme_resources_enabled = true;
    const auto untrusted = fix.load(std::move(untrusted_request));
    CHECK(untrusted.resources.project_themes.empty());
    CHECK(untrusted.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);

    coding_agent::ProjectResourceLoadingRequest trusted_request;
    trusted_request.theme_resources_enabled = true;
    trusted_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    const auto trusted = fix.load(std::move(trusted_request));
    REQUIRE(trusted.resources.project_themes.size() == 1);
    CHECK(trusted.resources.project_themes[0].path == ".pi/themes/project.json");
    CHECK(trusted.resources.project_themes[0].json == valid_theme_json());
}

TEST_CASE("project resource loader skips untrusted resources before parsing adapters", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    fix.write(
        ".pi/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "this line has no colon\n"
        "description: Bad skill.\n"
        "---\n"
        "Body.\n");
    fix.write(
        ".pi/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    auto result = fix.load();

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    CHECK_FALSE(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "bad line"));
    CHECK_FALSE(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "description"));
}

TEST_CASE("project resource loader treats marker presence as trust-requiring under --no-skills", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    write_valid_project_prompt(fix);

    coding_agent::ProjectResourceLoadingRequest request;
    request.no_skills = true;

    auto result = fix.load(std::move(request));

    // pi semantics: marker presence triggers the trust decision regardless of
    // the no-* flags; the flags only gate which adapters load.
    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
}

TEST_CASE("project resource loader --no-skills drops skills discovery only", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    write_valid_project_prompt(fix);

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.no_skills = true;

    auto result = fix.load(std::move(request));

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(result.resources.skills.empty());
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "review");
}

TEST_CASE("project resource loader --no-prompt-templates drops discovery but keeps explicit inputs", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_prompt(fix, "shared");
    fix.write(
        "explicit.md",
        "---\n"
        "description: Explicit prompt.\n"
        "---\n"
        "Explicit body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.no_prompt_templates = true;
    request.explicit_prompt_templates.push_back({.path = "explicit.md", .is_file = true});

    auto result = fix.load(std::move(request));

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "explicit");
}

TEST_CASE("project resource loader loads explicit prompt template inputs apart from project markers", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    fix.write(
        "explicit.md",
        "---\n"
        "description: Explicit prompt.\n"
        "---\n"
        "Explicit body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.explicit_prompt_templates.push_back({.path = "explicit.md", .is_file = true});

    auto result = fix.load(std::move(request));

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "explicit");
    CHECK(result.resources.skills.empty());
}

TEST_CASE("project resource loader explicit prompt templates take precedence over project prompts", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_prompt(fix, "shared");
    fix.write(
        "shared.md",
        "---\n"
        "description: Explicit prompt.\n"
        "---\n"
        "Explicit body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.explicit_prompt_templates.push_back({.path = "shared.md", .is_file = true});

    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "shared");
    CHECK(result.resources.prompt_templates[0].content.find("Explicit body") != std::string::npos);
    CHECK(result.resources.prompt_templates[0].content.find("Project prompt body") == std::string::npos);
    REQUIRE(has_diag(result, coding_agent::ResourceDiagnosticType::Collision, "name \"/shared\" collision"));
    const auto collision = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diag) { return diag.type == coding_agent::ResourceDiagnosticType::Collision; });
    REQUIRE(collision != result.diagnostics.end());
    REQUIRE(collision->collision.has_value());
    CHECK(collision->collision->resource_type == coding_agent::ResourceCollisionResourceType::Prompt);
    CHECK(collision->collision->name == "shared");
    CHECK(collision->collision->winner_path.find("shared.md") != std::string::npos);
    CHECK(collision->collision->loser_path.find(".pi/prompts/shared.md") != std::string::npos);
}

TEST_CASE("project resource loader loads user prompt templates below project precedence", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_prompt(fix, "shared");
    tests::TempWorkspace agent_dir;
    agent_dir.write("prompts/shared.md",
        "---\n"
        "description: User prompt.\n"
        "---\n"
        "User body.\n");
    agent_dir.write("prompts/only-user.md",
        "---\n"
        "description: User-only prompt.\n"
        "---\n"
        "User-only body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.agent_config_directory = agent_dir.path();

    auto result = fix.load(std::move(request));

    // User templates load; a project template with the same name wins.
    REQUIRE(result.resources.prompt_templates.size() == 2);
    const auto shared = std::find_if(
        result.resources.prompt_templates.begin(),
        result.resources.prompt_templates.end(),
        [](const auto& tmpl) { return tmpl.name == "shared"; });
    REQUIRE(shared != result.resources.prompt_templates.end());
    CHECK(shared->content.find("Project prompt body") != std::string::npos);
    const auto user_only = std::find_if(
        result.resources.prompt_templates.begin(),
        result.resources.prompt_templates.end(),
        [](const auto& tmpl) { return tmpl.name == "only-user"; });
    REQUIRE(user_only != result.resources.prompt_templates.end());
    CHECK(user_only->filePath.find("prompts/only-user.md") != std::string::npos);
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Collision, "name \"/shared\" collision"));
}

TEST_CASE("project resource loader user prompt templates load without project trust", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    tests::TempWorkspace agent_dir;
    agent_dir.write("prompts/user-only.md",
        "---\n"
        "description: User prompt.\n"
        "---\n"
        "User body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.agent_config_directory = agent_dir.path();

    auto result = fix.load(std::move(request));

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "user-only");
}

TEST_CASE("project resource loader surfaces adapter diagnostics for malformed trusted inputs", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    fix.write(
        ".pi/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "description:\n"
        "---\n"
        "Body.\n");
    fix.write(
        ".pi/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "description is required"));
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "YAML frontmatter parse error"));
}

TEST_CASE("project resource loader diagnoses skill name collisions with winner and loser paths", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    fix.write(
        ".pi/skills/first/SKILL.md",
        "---\n"
        "name: dupe-skill\n"
        "description: First skill.\n"
        "---\n"
        "First skill body.\n");
    fix.write(
        ".pi/skills/second/SKILL.md",
        "---\n"
        "name: dupe-skill\n"
        "description: Duplicate skill.\n"
        "---\n"
        "Duplicate skill body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.skills.size() == 1);
    // The winner is the first loaded skill (its file path identifies it; the
    // body is read at invocation time, not preloaded).
    CHECK(result.resources.skills[0].filePath.find(".pi/skills/first/SKILL.md") != std::string::npos);
    CHECK(result.resources.skills[0].baseDir.find(".pi/skills/first") != std::string::npos);
    CHECK(result.resources.skills[0].sourceInfo.scope == coding_agent::SourceScope::Project);
    CHECK(result.resources.skills[0].sourceInfo.base_dir.value_or("").find(".pi") != std::string::npos);
    REQUIRE(count_diag(result, coding_agent::ResourceDiagnosticType::Collision) == 1);
    const auto& diagnostic = *std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diag) { return diag.type == coding_agent::ResourceDiagnosticType::Collision; });
    CHECK(diagnostic.message == "name \"dupe-skill\" collision");
    REQUIRE(diagnostic.collision.has_value());
    CHECK(diagnostic.collision->resource_type == coding_agent::ResourceCollisionResourceType::Skill);
    CHECK(diagnostic.collision->name == "dupe-skill");
    CHECK(diagnostic.collision->winner_path.find(".pi/skills/first/SKILL.md") != std::string::npos);
    CHECK(diagnostic.collision->loser_path.find(".pi/skills/second/SKILL.md") != std::string::npos);
}

TEST_CASE("project resource loader diagnoses prompt collisions within one project directory", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    fix.write(
        ".pi/prompts/dupe.md",
        "---\n"
        "description: First prompt.\n"
        "---\n"
        "First prompt body.\n");
    fix.write(
        ".pi/prompts/dupe.MD",
        "---\n"
        "description: Duplicate prompt.\n"
        "---\n"
        "Duplicate prompt body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "dupe");
    REQUIRE(count_diag(result, coding_agent::ResourceDiagnosticType::Collision) == 1);
    const auto& diagnostic = *std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diag) { return diag.type == coding_agent::ResourceDiagnosticType::Collision; });
    REQUIRE(diagnostic.collision.has_value());
    CHECK(diagnostic.collision->resource_type == coding_agent::ResourceCollisionResourceType::Prompt);
    CHECK(diagnostic.collision->name == "dupe");
}

TEST_CASE("project resource loader surfaces trust store failures as pi-shaped warnings", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    fix.write("trust.json", "{not json");

    auto result = fix.load();

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "failed to parse trust store"));
    const auto diag = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& d) { return d.message.find("trust store") != std::string::npos; });
    REQUIRE(diag != result.diagnostics.end());
    REQUIRE(diag->path.has_value());
    CHECK(diag->path->find("trust.json") != std::string::npos);
}

TEST_CASE(
    "project resource diagnostics redact before shared bounded truncation",
    "[coding_agent][project-resource-loader][issue72]") {
    LoaderFixture fix;
    const std::string secret = "sk-resource-diagnostic-secret-123456";
    // A hostile frontmatter name that is otherwise loadable (lowercase,
    // digits, hyphens) so it reaches the pi collision diagnostic, whose
    // message echoes the name (pi `name \"<name>\" collision`).
    const std::string hostile_name = secret + std::string(1100, 'x');
    for (const auto& dir : {"first", "second"}) {
        fix.write(
            ".pi/skills/" + std::string{dir} + "/SKILL.md",
            "---\nname: " + hostile_name + "\ndescription: Hostile metadata.\n---\nBody.\n");
    }

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    const auto result = fix.load(std::move(request));

    // The collision diagnostic includes the hostile frontmatter name.
    const auto collision_diag = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& d) {
            return d.type == coding_agent::ResourceDiagnosticType::Collision;
        });
    REQUIRE(collision_diag != result.diagnostics.end());
    CHECK(collision_diag->message.find(secret) == std::string::npos);
    CHECK(collision_diag->message.find("[REDACTED]") != std::string::npos);
    CHECK(collision_diag->message.size() <= 1024);
}

TEST_CASE(
    "project resource diagnostic paths are bounded at the shared seam",
    "[coding_agent][project-resource-loader][issue72]") {
    LoaderFixture fix;
    const std::string long_dir = std::string(200, 'a') + "/" +
        std::string(200, 'b') + "/" + std::string(200, 'c') + "/" +
        std::string(200, 'd') + "/" + std::string(200, 'e') + "/" +
        std::string(200, 'f');
    fix.write(
        ".pi/skills/" + long_dir + "/SKILL.md",
        "---\nname: deep-skill\ndescription: " + std::string(1100, 'd') + "\n---\nBody.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    const auto result = fix.load(std::move(request));

    REQUIRE(!result.diagnostics.empty());
    for (const auto& diagnostic : result.diagnostics) {
        CHECK(diagnostic.message.size() <= 1024);
        if (diagnostic.path) CHECK(diagnostic.path->size() <= 1024);
    }
}

TEST_CASE("project resource loader treats explicit prompt template read failure as fatal", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    // No file named missing.md exists.
    coding_agent::ProjectResourceLoadingRequest request;
    request.explicit_prompt_templates.push_back({.path = "missing.md", .is_file = true});

    auto result = fix.load(std::move(request));

    REQUIRE(!result.fatal_errors.empty());
    CHECK(result.fatal_errors[0].type == coding_agent::ResourceDiagnosticType::Error);
    CHECK(result.fatal_errors[0].message.find("could not open file") != std::string::npos ||
          result.fatal_errors[0].message.find("not found") != std::string::npos ||
          result.fatal_errors[0].message.find("could not read") != std::string::npos);
    CHECK(result.resources.prompt_templates.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("project resource loader treats explicit non-markdown input as fatal", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    fix.write("notes.txt", "not a template");

    coding_agent::ProjectResourceLoadingRequest request;
    request.explicit_prompt_templates.push_back({.path = "notes.txt", .is_file = true});

    auto result = fix.load(std::move(request));

    REQUIRE(!result.fatal_errors.empty());
    CHECK(result.fatal_errors[0].type == coding_agent::ResourceDiagnosticType::Error);
    CHECK(result.fatal_errors[0].message.find(".md extension") != std::string::npos);
}

TEST_CASE("project resource loader rejects legacy .cpp-harness/ markers without loading them", "[coding_agent][project-resource-loader][issue405]") {
    LoaderFixture fix;
    // A legacy marker tree must be invisible: no trust trigger, no load.
    fix.write(
        ".cpp-harness/skills/legacy/SKILL.md",
        "---\n"
        "name: legacy\n"
        "description: Legacy skill.\n"
        "---\n"
        "Legacy body.\n");
    fix.write(
        ".cpp-harness/prompts/legacy.md",
        "---\n"
        "description: Legacy prompt.\n"
        "---\n"
        "Legacy body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    CHECK(result.diagnostics.empty());
}

// ── P16: skill discovery — user ~/.pi/agent/skills, .agents/skills, --skill ──

TEST_CASE(
    "project resource loader loads user skills with pi root-level .md inclusion",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;
    tests::TempWorkspace agent_dir;
    agent_dir.write("skills/user-skill/SKILL.md",
        "---\n"
        "name: user-skill\n"
        "description: User skill.\n"
        "---\n"
        "User body.\n");
    // pi discovery mode: root-level .md files in ~/.pi/agent/skills load.
    agent_dir.write("skills/root-skill.md",
        "---\n"
        "name: root-skill\n"
        "description: Root-level user skill.\n"
        "---\n"
        "Root body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.agent_config_directory = agent_dir.path();

    auto result = fix.load(std::move(request));

    // No project markers: no trust decision involved, user skills load.
    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    REQUIRE(result.resources.skills.size() == 2);
    const auto root_skill = std::find_if(
        result.resources.skills.begin(),
        result.resources.skills.end(),
        [](const auto& skill) { return skill.name == "root-skill"; });
    REQUIRE(root_skill != result.resources.skills.end());
    CHECK(root_skill->sourceInfo.scope == coding_agent::SourceScope::User);
    CHECK(root_skill->sourceInfo.source == "auto");
    CHECK(root_skill->sourceInfo.base_dir == agent_dir.path().string());
    // The file body is not preloaded; the base directory is the file's own.
    CHECK(root_skill->baseDir.find("skills") != std::string::npos);
    CHECK(root_skill->filePath.find("skills/root-skill.md") != std::string::npos);
}

TEST_CASE(
    "project resource loader loads project .pi/skills root-level .md files (pi mode)",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;
    fix.write(
        ".pi/skills/root-skill.md",
        "---\n"
        "name: root-skill\n"
        "description: Project root-level skill.\n"
        "---\n"
        "Root body.\n");
    fix.write(
        ".pi/skills/nested/skill/SKILL.md",
        "---\n"
        "name: nested-skill\n"
        "description: Nested project skill.\n"
        "---\n"
        "Nested body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.skills.size() == 2);
    const auto root_skill = std::find_if(
        result.resources.skills.begin(),
        result.resources.skills.end(),
        [](const auto& skill) { return skill.name == "root-skill"; });
    REQUIRE(root_skill != result.resources.skills.end());
    CHECK(root_skill->sourceInfo.scope == coding_agent::SourceScope::Project);
    CHECK(root_skill->sourceInfo.base_dir.value_or("").find(".pi") != std::string::npos);
}

TEST_CASE(
    "project resource loader loads .agents/skills with per-directory baseDir",
    "[coding_agent][project-resource-loader][issue412]") {
    // A git-rooted workspace with a nested project directory: the ancestor
    // walk covers the workspace and the git root.
    tests::TempWorkspace git_root;
    std::filesystem::create_directories(git_root.path() / ".git");
    std::filesystem::create_directories(git_root.path() / "proj");
    const auto workspace = git_root.path() / "proj";
    auto fs = harness::WorkspaceFileSystem::create(workspace);
    REQUIRE(fs.has_value());
    git_root.write(".agents/skills/repo-skill/SKILL.md",
        "---\n"
        "name: repo-skill\n"
        "description: Repo .agents skill.\n"
        "---\n"
        "Repo body.\n");

    coding_agent::ProjectTrustStore trust_store{workspace / "trust.json"};
    coding_agent::ProjectResourceLoadingRequest request;
    request.workspace = workspace;
    request.home_directory = git_root.path() / "home";
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    auto result = coding_agent::load_project_resources(*fs, trust_store, std::move(request));

    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].name == "repo-skill");
    // The per-directory baseDir is the owning .agents directory.
    CHECK(result.resources.skills[0].sourceInfo.scope == coding_agent::SourceScope::Project);
    CHECK(result.resources.skills[0].sourceInfo.source == "auto");
    CHECK(result.resources.skills[0].sourceInfo.base_dir.value_or("").find(".agents") != std::string::npos);
    // The skill's own base directory remains the SKILL.md parent.
    CHECK(result.resources.skills[0].baseDir.find("repo-skill") != std::string::npos);
}

TEST_CASE(
    "project resource loader .agents/skills presence triggers the trust decision",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;
    fix.write(
        ".agents/skills/proj-skill/SKILL.md",
        "---\n"
        "name: proj-skill\n"
        "description: Project .agents skill.\n"
        "---\n"
        "Body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.home_directory = fix.workspace.path() / "home";

    auto result = fix.load(std::move(request));

    // Mere presence gates loading: untrusted → nothing loads.
    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(result.resources.skills.empty());
}

TEST_CASE(
    "project resource loader loads the user ~/.agents/skills convention without trust",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;
    tests::TempWorkspace home;
    home.write(".agents/skills/user-agents-skill/SKILL.md",
        "---\n"
        "name: user-agents-skill\n"
        "description: User .agents skill.\n"
        "---\n"
        "Body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.home_directory = home.path();

    auto result = fix.load(std::move(request));

    // The user's own ~/.agents/skills is always trusted and never triggers
    // the project trust decision.
    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].name == "user-agents-skill");
    CHECK(result.resources.skills[0].sourceInfo.scope == coding_agent::SourceScope::User);
    CHECK(result.resources.skills[0].sourceInfo.base_dir.value_or("").find(".agents") != std::string::npos);
}

TEST_CASE(
    "project resource loader --skill explicit paths load first and survive --no-skills",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;
    fix.write(
        "explicit-skill.md",
        "---\n"
        "name: explicit-skill\n"
        "description: Explicit skill.\n"
        "---\n"
        "Explicit body.\n");
    fix.write(
        ".pi/skills/explicit-skill/SKILL.md",
        "---\n"
        "name: explicit-skill\n"
        "description: Discovered duplicate.\n"
        "---\n"
        "Discovered body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.no_skills = true;
    request.skill_paths.push_back("explicit-skill.md");

    auto result = fix.load(std::move(request));

    // --no-skills drops discovery but keeps the explicit path.
    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].name == "explicit-skill");
    CHECK(result.resources.skills[0].filePath.find("explicit-skill.md") != std::string::npos);
    CHECK(result.resources.skills[0].sourceInfo.source == "cli");
    CHECK(result.resources.skills[0].sourceInfo.scope == coding_agent::SourceScope::Temporary);
    CHECK_FALSE(result.resources.skills[0].sourceInfo.base_dir.has_value());
}

TEST_CASE(
    "project resource loader discovered skills win name collisions over explicit --skill",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;
    fix.write(
        "explicit-dir/skill/SKILL.md",
        "---\n"
        "name: shared\n"
        "description: Explicit skill.\n"
        "---\n"
        "Explicit body.\n");
    fix.write(
        ".pi/skills/shared/SKILL.md",
        "---\n"
        "name: shared\n"
        "description: Discovered skill.\n"
        "---\n"
        "Discovered body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.skill_paths.push_back("explicit-dir/skill/SKILL.md");

    auto result = fix.load(std::move(request));

    // pi resource-loader.ts `mergePaths` appends the --skill paths after the
    // discovered ones, and `loadSkills` is first-wins: the discovered skill
    // wins and the explicit path loses with the collision diagnostic.
    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].filePath.find(".pi/skills/shared/SKILL.md") != std::string::npos);
    REQUIRE(has_diag(result, coding_agent::ResourceDiagnosticType::Collision, "name \"shared\" collision"));
    const auto collision = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diag) { return diag.type == coding_agent::ResourceDiagnosticType::Collision; });
    REQUIRE(collision != result.diagnostics.end());
    REQUIRE(collision->collision.has_value());
    CHECK(collision->collision->winner_path.find(".pi/skills/shared/SKILL.md") != std::string::npos);
    CHECK(collision->collision->loser_path.find("explicit-dir/skill/SKILL.md") != std::string::npos);
}

TEST_CASE(
    "project resource loader missing --skill paths carry pi's two diagnostics",
    "[coding_agent][project-resource-loader][issue412]") {
    LoaderFixture fix;

    coding_agent::ProjectResourceLoadingRequest request;
    request.skill_paths.push_back("missing-skill.md");

    auto result = fix.load(std::move(request));

    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "skill path does not exist"));
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Error, "Skill path does not exist"));
    CHECK(result.resources.skills.empty());
    CHECK(result.fatal_errors.empty());
}
