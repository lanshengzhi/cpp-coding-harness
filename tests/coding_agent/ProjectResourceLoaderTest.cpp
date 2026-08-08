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
    CHECK(result.resources.skills[0].content.find("First skill body") != std::string::npos);
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
    const std::string hostile_name = secret + std::string(1100, 'x');
    fix.write(
        ".pi/skills/safe/SKILL.md",
        "---\nname: " + hostile_name + "\ndescription: Hostile metadata.\n---\nBody.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    const auto result = fix.load(std::move(request));

    REQUIRE(!result.diagnostics.empty());

    // The name mismatch diagnostic includes the hostile frontmatter name.
    const auto secret_diag = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& d) {
            return d.message.find("does not match parent directory") != std::string::npos;
        });
    REQUIRE(secret_diag != result.diagnostics.end());
    CHECK(secret_diag->message.find(secret) == std::string::npos);
    CHECK(secret_diag->message.find("[REDACTED]") != std::string::npos);
    CHECK(secret_diag->message.size() <= 1024);
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
        "---\nname: deep-skill\ndescription: Deep.\n---\nBody.\n");

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
