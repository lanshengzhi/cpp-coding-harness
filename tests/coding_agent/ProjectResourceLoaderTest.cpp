#include "coding_agent/ProjectResourceLoader.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

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
    "project resource loader collects trust-gated project themes and skips them with --no-themes",
    "[coding_agent][project-resource-loader][theme][issue405][issue415]") {
    LoaderFixture fix;
    fix.write(".pi/themes/project.json", valid_theme_json());

    coding_agent::ProjectResourceLoadingRequest untrusted_request;
    const auto untrusted = fix.load(std::move(untrusted_request));
    CHECK(untrusted.resources.themes.empty());
    CHECK(untrusted.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);

    coding_agent::ProjectResourceLoadingRequest trusted_request;
    trusted_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    const auto trusted = fix.load(std::move(trusted_request));
    REQUIRE(trusted.resources.themes.size() == 1);
    CHECK(trusted.resources.themes[0].path == ".pi/themes/project.json");
    CHECK(trusted.resources.themes[0].json == valid_theme_json());
    CHECK(trusted.resources.themes[0].scope == coding_agent::SourceScope::Project);

    // pi `--no-themes`: drops auto-discovery of the default directories
    // (user `<agent_config_directory>/themes` and project `.pi/themes`).
    coding_agent::ProjectResourceLoadingRequest no_themes_request;
    no_themes_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    no_themes_request.no_themes = true;
    const auto no_themes = fix.load(std::move(no_themes_request));
    CHECK(no_themes.resources.themes.empty());
}

TEST_CASE(
    "project resource loader collects user themes from the agent config directory",
    "[coding_agent][project-resource-loader][theme][issue415]") {
    LoaderFixture fix;
    tests::TempWorkspace agent_config;
    agent_config.write("themes/user.json", valid_theme_json());
    agent_config.write("themes/notes.txt", "not a theme");

    coding_agent::ProjectResourceLoadingRequest request;
    request.agent_config_directory = agent_config.path();
    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.themes.size() == 1);
    CHECK(result.resources.themes[0].path.find("themes/user.json") != std::string::npos);
    CHECK(result.resources.themes[0].json == valid_theme_json());
    CHECK(result.resources.themes[0].scope == coding_agent::SourceScope::User);
    // A missing user themes directory loads nothing silently.
    tests::TempWorkspace empty_config;
    coding_agent::ProjectResourceLoadingRequest empty_request;
    empty_request.agent_config_directory = empty_config.path();
    const auto empty = fix.load(std::move(empty_request));
    CHECK(empty.resources.themes.empty());
    CHECK(empty.diagnostics.empty());
}

TEST_CASE(
    "project resource loader --theme explicit paths load directories and files",
    "[coding_agent][project-resource-loader][theme][issue415]") {
    LoaderFixture fix;
    fix.write("explicit-dir/one.json", valid_theme_json());
    fix.write("explicit-dir/two.json", valid_theme_json());
    fix.write("explicit.json", valid_theme_json());
    fix.write("not-theme.txt", "not json");

    coding_agent::ProjectResourceLoadingRequest request;
    request.theme_paths = {"explicit.json", "explicit-dir", "not-theme.txt"};
    auto result = fix.load(std::move(request));

    // Explicit paths stay effective without trust and collect in CLI order.
    REQUIRE(result.resources.themes.size() == 3);
    CHECK(result.resources.themes[0].path == "explicit.json");
    CHECK(result.resources.themes[0].scope == coding_agent::SourceScope::Temporary);
    CHECK(result.resources.themes[1].path == "explicit-dir/one.json");
    CHECK(result.resources.themes[2].path == "explicit-dir/two.json");
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "theme path is not a json file"));

    // `--no-themes` drops discovery but keeps explicit paths.
    coding_agent::ProjectResourceLoadingRequest no_themes_request;
    no_themes_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    no_themes_request.no_themes = true;
    no_themes_request.theme_paths = {"explicit.json"};
    const auto no_themes = fix.load(std::move(no_themes_request));
    REQUIRE(no_themes.resources.themes.size() == 1);
    CHECK(no_themes.resources.themes[0].path == "explicit.json");
}

TEST_CASE(
    "project resource loader missing --theme paths carry pi's two non-fatal diagnostics",
    "[coding_agent][project-resource-loader][theme][issue415]") {
    LoaderFixture fix;

    coding_agent::ProjectResourceLoadingRequest request;
    request.theme_paths = {"missing-theme.json"};
    auto result = fix.load(std::move(request));

    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning, "theme path does not exist"));
    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Error, "Theme path does not exist"));
    CHECK(result.resources.themes.empty());
    CHECK(result.fatal_errors.empty());
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
    CHECK((result.fatal_errors[0].message.find("could not open file") != std::string::npos ||
           result.fatal_errors[0].message.find("not found") != std::string::npos ||
           result.fatal_errors[0].message.find("could not read") != std::string::npos));
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

// ─────────────────────────────────────────────────────────────────────────────
// P20 (#416): Project Context Files and SYSTEM.md/APPEND_SYSTEM.md
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE(
    "project resource loader discovers project context files from the global dir and the cwd ancestor chain",
    "[coding_agent][project-resource-loader][context-files][issue416]") {
    LoaderFixture fix;
    // Global context file from the Agent Config Directory.
    tests::TempWorkspace agent_config;
    agent_config.write("AGENTS.md", "global instructions\n");

    // Ancestor chain: repo (AGENTS.md) -> sub (AGENTS.MD) -> deep (AGENTS.md
    // wins over CLAUDE.md in the same directory).
    fix.write("repo/AGENTS.md", "repo instructions\n");
    fix.write("repo/sub/AGENTS.MD", "sub instructions\n");
    fix.write("repo/sub/deep/CLAUDE.md", "deep claude instructions\n");
    fix.write("repo/sub/deep/AGENTS.md", "deep instructions\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.agent_config_directory = agent_config.path();
    request.workspace = fix.workspace.path() / "repo" / "sub" / "deep";
    auto result =
        coding_agent::load_project_resources(fix.fs, fix.trust_store, std::move(request));

    // pi `loadProjectContextFiles` order: the global file first, then the
    // ancestor chain root-most first (the cwd-ward walk unshifts each find).
    REQUIRE(result.resources.agents_files.size() == 4);
    CHECK(result.resources.agents_files[0].path == (agent_config.path() / "AGENTS.md").string());
    CHECK(result.resources.agents_files[0].content == "global instructions\n");
    CHECK(result.resources.agents_files[1].path == (fix.workspace.path() / "repo" / "AGENTS.md").string());
    CHECK(result.resources.agents_files[1].content == "repo instructions\n");
    CHECK(result.resources.agents_files[2].path == (fix.workspace.path() / "repo" / "sub" / "AGENTS.MD").string());
    CHECK(result.resources.agents_files[2].content == "sub instructions\n");
    // AGENTS.md wins over CLAUDE.md as the first candidate in the same dir.
    CHECK(result.resources.agents_files[3].path == (fix.workspace.path() / "repo" / "sub" / "deep" / "AGENTS.md").string());
    CHECK(result.resources.agents_files[3].content == "deep instructions\n");
}

TEST_CASE(
    "project resource loader context files are not trust-gated and --no-context-files disables discovery",
    "[coding_agent][project-resource-loader][context-files][issue416]") {
    LoaderFixture fix;
    fix.write("AGENTS.md", "workspace instructions\n");

    // No trust markers, so the decision is Trusted without a prompt; the
    // context file loads either way (pinned fact: Project Context Files are
    // never Project Trust gated).
    auto untrusted = fix.load();
    CHECK(untrusted.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    REQUIRE(untrusted.resources.agents_files.size() == 1);
    CHECK(untrusted.resources.agents_files[0].content == "workspace instructions\n");

    // pi `--no-context-files`: discovery disabled entirely.
    coding_agent::ProjectResourceLoadingRequest disabled;
    disabled.no_context_files = true;
    auto result = fix.load(std::move(disabled));
    CHECK(result.resources.agents_files.empty());
}

/// Builds a linked-worktree skeleton (no git binary needed): the main
/// repo's `.git/worktrees/<name>/` holds `HEAD` plus a `commondir` pointing
/// back at the main `.git`, and the worktree's working tree carries a `.git`
/// *file* whose `gitdir:` resolves to it (pi `resource-loader.test.ts`
/// `linkWorktree`).
void link_worktree(
    const LoaderFixture& fix,
    const std::filesystem::path& main_dir,
    const std::filesystem::path& worktree_dir,
    const std::string& name) {
    const auto git_dir = main_dir / ".git" / "worktrees" / name;
    fix.write((main_dir / ".git" / "HEAD").string(), "ref: refs/heads/main\n");
    fix.write((git_dir / "HEAD").string(), "ref: refs/heads/feat\n");
    fix.write((git_dir / "commondir").string(), "../..");
    fix.write(
        (worktree_dir / ".git").string(),
        "gitdir: " + git_dir.string() + "\n");
}

[[nodiscard]] coding_agent::ProjectResourceLoadingResult load_context_files_at(
    const LoaderFixture& fix,
    const std::filesystem::path& workspace) {
    coding_agent::ProjectResourceLoadingRequest request;
    request.workspace = workspace;
    return coding_agent::load_project_resources(fix.fs, fix.trust_store, std::move(request));
}

[[nodiscard]] std::vector<std::string> context_contents(
    const coding_agent::ProjectResourceLoadingResult& result) {
    std::vector<std::string> contents;
    for (const auto& file : result.resources.agents_files) {
        contents.push_back(file.content);
    }
    return contents;
}

TEST_CASE(
    "project resource loader context files load even under an untrusted decision",
    "[coding_agent][project-resource-loader][context-files][issue416]") {
    LoaderFixture fix;
    // A trust-requiring marker forces an Untrusted decision (default ask),
    // yet the context file still loads: Project Context Files are never
    // Project Trust gated (pinned fact).
    write_valid_project_skill(fix);
    fix.write("AGENTS.md", "workspace instructions\n");

    auto result = fix.load();
    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    REQUIRE(result.resources.agents_files.size() == 1);
    CHECK(result.resources.agents_files[0].content == "workspace instructions\n");
    // The trust decision still gates the project resources.
    CHECK(result.resources.skills.empty());
}

TEST_CASE(
    "project resource loader dedupes a global context file that is also an ancestor",
    "[coding_agent][project-resource-loader][context-files][issue416]") {
    LoaderFixture fix;
    fix.write("repo/AGENTS.md", "repo instructions\n");

    coding_agent::ProjectResourceLoadingRequest request;
    // The Agent Config Directory is an ancestor of the workspace: the global
    // file and the ancestor file are the same path and load once.
    request.agent_config_directory = fix.workspace.path() / "repo";
    request.workspace = fix.workspace.path() / "repo" / "sub";
    auto result =
        coding_agent::load_project_resources(fix.fs, fix.trust_store, std::move(request));

    REQUIRE(result.resources.agents_files.size() == 1);
    CHECK(result.resources.agents_files[0].content == "repo instructions\n");
}

TEST_CASE(
    "project resource loader shadows the main repo context file in a nested linked worktree",
    "[coding_agent][project-resource-loader][context-files][issue416]") {
    LoaderFixture fix;
    // Fake git layout: a main repo at repo/ and a nested linked worktree at
    // repo/wt (the `.git` file's `gitdir:` pointer + commondir like `git
    // worktree add` writes them).
    fix.write("repo/.git/HEAD", "ref: refs/heads/main\n");
    fix.write("repo/.git/worktrees/wt/HEAD", "ref: refs/heads/main\n");
    fix.write("repo/.git/worktrees/wt/commondir", "../..\n");
    fix.write(
        "repo/wt/.git",
        "gitdir: " +
            (fix.workspace.path() / "repo" / ".git" / "worktrees" / "wt").string() +
            "\n");
    fix.write("repo/AGENTS.md", "main repo copy\n");
    fix.write("repo/wt/AGENTS.md", "worktree copy\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.workspace = fix.workspace.path() / "repo" / "wt";
    auto result =
        coding_agent::load_project_resources(fix.fs, fix.trust_store, std::move(request));

    // The worktree's own copy wins; the main repo's copy of the same tracked
    // file is shadowed and loads once.
    REQUIRE(result.resources.agents_files.size() == 1);
    CHECK(result.resources.agents_files[0].path ==
          (fix.workspace.path() / "repo" / "wt" / "AGENTS.md").string());
    CHECK(result.resources.agents_files[0].content == "worktree copy\n");
}

TEST_CASE(
    "project resource loader worktree shadowing matches pi's dedupe cases",
    "[coding_agent][project-resource-loader][context-files][issue416]") {
    // 1. The worktree root has no context file: the main repo's copy loads
    // (shadowing needs the worktree's own copy to exist).
    {
        LoaderFixture fix;
        const auto main = fix.workspace.path() / "repo";
        const auto wt = main / "worktrees" / "feat";
        const auto src = wt / "src";
        fix.write("repo/AGENTS.md", "main repo instructions");
        link_worktree(fix, main, wt, "feat");
        auto result = load_context_files_at(fix, src);
        CHECK(context_contents(result) == std::vector<std::string>{"main repo instructions"});
    }
    // 2. Only the same filename is shadowed: the worktree tracks AGENTS.md
    // while the main repo tracks CLAUDE.md, so the main repo's CLAUDE.md is
    // nobody's duplicate and still loads.
    {
        LoaderFixture fix;
        const auto main = fix.workspace.path() / "repo";
        const auto wt = main / "worktrees" / "feat";
        const auto src = wt / "src";
        fix.write("repo/CLAUDE.md", "main repo instructions");
        fix.write("repo/worktrees/feat/AGENTS.md", "worktree instructions");
        link_worktree(fix, main, wt, "feat");
        auto result = load_context_files_at(fix, src);
        CHECK((context_contents(result) ==
              std::vector<std::string>{"main repo instructions", "worktree instructions"}));
    }
    // 3. An ordinary repo root (plain `.git` directory) keeps climbing: the
    // repo root and the worktree root are the same dir, so nothing is
    // shadowed.
    {
        LoaderFixture fix;
        fix.write("outer/repo/.git/HEAD", "ref: refs/heads/main\n");
        fix.write("outer/AGENTS.md", "outer instructions");
        fix.write("outer/repo/AGENTS.md", "repo instructions");
        fix.write("outer/repo/src/AGENTS.md", "leaf instructions");
        auto result = load_context_files_at(fix, fix.workspace.path() / "outer" / "repo" / "src");
        CHECK((context_contents(result) ==
              std::vector<std::string>{"outer instructions", "repo instructions", "leaf instructions"}));
    }
    // 4. A sibling worktree (main repo not an ancestor) shadows nothing.
    {
        LoaderFixture fix;
        const auto main = fix.workspace.path() / "outer" / "main";
        const auto sib = fix.workspace.path() / "outer" / "sib-feat";
        const auto sib_src = sib / "src";
        fix.write("outer/AGENTS.md", "outer instructions");
        fix.write("outer/sib-feat/AGENTS.md", "sibling worktree instructions");
        link_worktree(fix, main, sib, "sib");
        auto result = load_context_files_at(fix, sib_src);
        CHECK((context_contents(result) ==
              std::vector<std::string>{"outer instructions", "sibling worktree instructions"}));
    }
    // 5. A bare layout (`proj/.bare` + `proj/main`): dirname of the common
    // git dir is the plain `proj` container, which tracks nothing, so its
    // context file is not a duplicate.
    {
        LoaderFixture fix;
        const auto proj = fix.workspace.path() / "proj";
        const auto bare = proj / ".bare";
        const auto wt = proj / "main";
        const auto wt_git_dir = bare / "worktrees" / "main";
        fix.write("proj/.bare/HEAD", "ref: refs/heads/main\n");
        fix.write("proj/.bare/worktrees/main/HEAD", "ref: refs/heads/main\n");
        fix.write("proj/.bare/worktrees/main/commondir", "../..");
        fix.write("proj/main/.git", "gitdir: " + wt_git_dir.string() + "\n");
        fix.write("proj/AGENTS.md", "container instructions");
        fix.write("proj/main/AGENTS.md", "worktree instructions");
        auto result = load_context_files_at(fix, wt);
        CHECK((context_contents(result) ==
              std::vector<std::string>{"container instructions", "worktree instructions"}));
    }
    // 6. A submodule's gitdir resolves under `.git/modules` — never an
    // ancestor of cwd — so the superproject's context loads alongside.
    {
        LoaderFixture fix;
        const auto sup = fix.workspace.path() / "super";
        const auto sub = sup / "vendor" / "lib";
        const auto sub_src = sub / "src";
        const auto sub_git_dir = sup / ".git" / "modules" / "vendor" / "lib";
        fix.write("super/AGENTS.md", "superproject instructions");
        fix.write("super/vendor/lib/AGENTS.md", "submodule instructions");
        fix.write("super/.git/modules/vendor/lib/HEAD", "ref: refs/heads/main\n");
        fix.write("super/vendor/lib/.git", "gitdir: " + sub_git_dir.string() + "\n");
        auto result = load_context_files_at(fix, sub_src);
        CHECK((context_contents(result) ==
              std::vector<std::string>{"superproject instructions", "submodule instructions"}));
    }
    // 7. A corrupt `gitdir:` target that does not exist keeps normal climbing.
    {
        LoaderFixture fix;
        fix.write("repo/.git", "gitdir: /nonexistent/path/worktrees/feat\n");
        fix.write("repo/AGENTS.md", "repo instructions");
        fix.write("repo/src/AGENTS.md", "src instructions");
        auto result = load_context_files_at(fix, fix.workspace.path() / "repo" / "src");
        CHECK((context_contents(result) ==
              std::vector<std::string>{"repo instructions", "src instructions"}));
    }
}

TEST_CASE(
    "project resource loader resolves SYSTEM.md trust-gated project file then global",
    "[coding_agent][project-resource-loader][system-prompt][issue416]") {
    LoaderFixture fix;
    tests::TempWorkspace agent_config;
    agent_config.write("SYSTEM.md", "global system prompt\n");
    fix.write(".pi/SYSTEM.md", "project system prompt\n");

    // Untrusted project: the `.pi/SYSTEM.md` marker triggers the trust
    // decision, the project file is skipped, and the global file loads.
    coding_agent::ProjectResourceLoadingRequest untrusted_request;
    untrusted_request.agent_config_directory = agent_config.path();
    auto untrusted = fix.load(std::move(untrusted_request));
    CHECK(untrusted.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    REQUIRE(untrusted.resources.system_prompt.has_value());
    CHECK(*untrusted.resources.system_prompt == "global system prompt\n");
    REQUIRE(untrusted.resources.system_prompt_source.has_value());
    CHECK(*untrusted.resources.system_prompt_source ==
          (agent_config.path() / "SYSTEM.md").string());

    // Trusted project: the project `.pi/SYSTEM.md` wins over the global.
    coding_agent::ProjectResourceLoadingRequest trusted_request;
    trusted_request.agent_config_directory = agent_config.path();
    trusted_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    auto trusted = fix.load(std::move(trusted_request));
    REQUIRE(trusted.resources.system_prompt.has_value());
    CHECK(*trusted.resources.system_prompt == "project system prompt\n");
    REQUIRE(trusted.resources.system_prompt_source.has_value());
    CHECK(*trusted.resources.system_prompt_source ==
          (fix.workspace.path() / ".pi" / "SYSTEM.md").string());
}

TEST_CASE(
    "project resource loader resolves APPEND_SYSTEM.md trust-gated project file then global",
    "[coding_agent][project-resource-loader][system-prompt][issue416]") {
    LoaderFixture fix;
    tests::TempWorkspace agent_config;
    agent_config.write("APPEND_SYSTEM.md", "global append\n");
    fix.write(".pi/APPEND_SYSTEM.md", "project append\n");

    // Untrusted project: the global file loads.
    coding_agent::ProjectResourceLoadingRequest untrusted_request;
    untrusted_request.agent_config_directory = agent_config.path();
    auto untrusted = fix.load(std::move(untrusted_request));
    REQUIRE(untrusted.resources.append_system_prompt.size() == 1);
    CHECK(untrusted.resources.append_system_prompt[0] == "global append\n");
    REQUIRE(untrusted.resources.append_system_prompt_sources.size() == 1);
    CHECK(untrusted.resources.append_system_prompt_sources[0] ==
          (agent_config.path() / "APPEND_SYSTEM.md").string());

    // Trusted project: the project `.pi/APPEND_SYSTEM.md` wins.
    coding_agent::ProjectResourceLoadingRequest trusted_request;
    trusted_request.agent_config_directory = agent_config.path();
    trusted_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    auto trusted = fix.load(std::move(trusted_request));
    REQUIRE(trusted.resources.append_system_prompt.size() == 1);
    CHECK(trusted.resources.append_system_prompt[0] == "project append\n");
}

TEST_CASE(
    "project resource loader --system-prompt and --append-system-prompt resolve text-or-file per pi",
    "[coding_agent][project-resource-loader][system-prompt][issue416]") {
    LoaderFixture fix;
    fix.write("custom.md", "custom file prompt\n");
    // Discovery must not win over the CLI values.
    fix.write(".pi/SYSTEM.md", "discovered prompt\n");
    fix.write(".pi/APPEND_SYSTEM.md", "discovered append\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.system_prompt = (fix.workspace.path() / "custom.md").string();
    request.append_system_prompt = {"first append", (fix.workspace.path() / "custom.md").string()};
    auto result = fix.load(std::move(request));

    // An existing path resolves to the file contents (pi `resolvePromptInput`);
    // a plain value stays the text.
    REQUIRE(result.resources.system_prompt.has_value());
    CHECK(*result.resources.system_prompt == "custom file prompt\n");
    REQUIRE(result.resources.append_system_prompt.size() == 2);
    CHECK(result.resources.append_system_prompt[0] == "first append");
    CHECK(result.resources.append_system_prompt[1] == "custom file prompt\n");
    REQUIRE(result.resources.system_prompt_source.has_value());
    CHECK(*result.resources.system_prompt_source ==
          (fix.workspace.path() / "custom.md").string());
    REQUIRE(result.resources.append_system_prompt_sources.size() == 1);
    CHECK(*result.resources.append_system_prompt_sources.begin() ==
          (fix.workspace.path() / "custom.md").string());
}

TEST_CASE(
    "project resource loader --system-prompt raw text wins over discovery and empty suppresses it",
    "[coding_agent][project-resource-loader][system-prompt][issue416]") {
    LoaderFixture fix;
    fix.write(".pi/SYSTEM.md", "discovered prompt\n");

    // Raw text: not a path, used verbatim, no source path recorded.
    coding_agent::ProjectResourceLoadingRequest text_request;
    text_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    text_request.system_prompt = "custom system prompt";
    auto text = fix.load(std::move(text_request));
    REQUIRE(text.resources.system_prompt.has_value());
    CHECK(*text.resources.system_prompt == "custom system prompt");
    CHECK_FALSE(text.resources.system_prompt_source.has_value());

    // pi: an empty `--system-prompt` value suppresses discovery entirely and
    // resolves to no custom prompt.
    coding_agent::ProjectResourceLoadingRequest empty_request;
    empty_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    empty_request.system_prompt = "";
    auto empty = fix.load(std::move(empty_request));
    CHECK_FALSE(empty.resources.system_prompt.has_value());

    // A missing path is treated as the prompt text itself (pi
    // `resolvePromptInput`), with no diagnostic.
    coding_agent::ProjectResourceLoadingRequest missing_request;
    missing_request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    missing_request.system_prompt = "missing/prompt.md";
    auto missing = fix.load(std::move(missing_request));
    REQUIRE(missing.resources.system_prompt.has_value());
    CHECK(*missing.resources.system_prompt == "missing/prompt.md");
    CHECK(missing.diagnostics.empty());
}

TEST_CASE(
    "project resource loader unreadable --system-prompt file warns and falls back to the raw value",
    "[coding_agent][project-resource-loader][system-prompt][issue416]") {
    LoaderFixture fix;
    fix.write("adir/note.txt", "x");

    // A directory exists but cannot be read as text: pi warns and uses the
    // value itself as the prompt text.
    coding_agent::ProjectResourceLoadingRequest request;
    request.system_prompt = (fix.workspace.path() / "adir").string();
    auto result = fix.load(std::move(request));

    CHECK(has_diag(result, coding_agent::ResourceDiagnosticType::Warning,
                   "Could not read system prompt file"));
    REQUIRE(result.resources.system_prompt.has_value());
    CHECK(*result.resources.system_prompt == (fix.workspace.path() / "adir").string());
    // pi `systemPromptSourcePath`: any existing source path is recorded —
    // the directory exists, so it is the recorded source even though the
    // read failed.
    REQUIRE(result.resources.system_prompt_source.has_value());
    CHECK(*result.resources.system_prompt_source ==
          (fix.workspace.path() / "adir").string());
}

TEST_CASE(
    "project resource loader no SYSTEM.md or APPEND_SYSTEM.md loads nothing",
    "[coding_agent][project-resource-loader][system-prompt][issue416]") {
    LoaderFixture fix;
    tests::TempWorkspace agent_config;

    coding_agent::ProjectResourceLoadingRequest request;
    request.agent_config_directory = agent_config.path();
    auto result = fix.load(std::move(request));

    CHECK_FALSE(result.resources.system_prompt.has_value());
    CHECK(result.resources.append_system_prompt.empty());
    CHECK(result.resources.system_prompt_source.has_value() == false);
    CHECK(result.resources.append_system_prompt_sources.empty());
    CHECK(result.diagnostics.empty());
}
