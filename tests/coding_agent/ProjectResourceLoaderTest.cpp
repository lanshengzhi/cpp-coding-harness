#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/ProjectResourceLoader.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <algorithm>
#include <filesystem>
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
        ".cpp-harness/skills/" + name + "/SKILL.md",
        "---\n"
        "name: " + name + "\n"
        "description: Project skill.\n"
        "---\n"
        "Project skill body.\n");
}

void write_valid_project_prompt(const LoaderFixture& fix, std::string name = "review") {
    fix.write(
        ".cpp-harness/prompts/" + name + ".md",
        "---\n"
        "description: Project prompt.\n"
        "---\n"
        "Project prompt body.\n");
}

const coding_agent::ResourceLoadDecision* find_decision(
    const coding_agent::ProjectResourceLoadPlan& plan,
    coding_agent::ProjectResourceKind kind) {
    auto it = std::find_if(
        plan.decisions.begin(),
        plan.decisions.end(),
        [kind](const auto& decision) { return decision.kind == kind; });
    return it == plan.decisions.end() ? nullptr : &*it;
}

void require_decision_reason(
    const coding_agent::ProjectResourceLoadPlan& plan,
    coding_agent::ProjectResourceKind kind,
    coding_agent::ResourceSkipReason reason) {
    const auto* decision = find_decision(plan, kind);
    REQUIRE(decision != nullptr);
    CHECK(decision->reason == reason);
}

bool has_diag_source(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ProjectResourceLoadingDiagnosticCategory category) {
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [category](const auto& diagnostic) { return diagnostic.category == category; });
}

bool has_diag(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ProjectResourceLoadingDiagnosticCategory category,
    std::string_view code) {
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [category, code](const auto& diagnostic) {
            return diagnostic.category == category && diagnostic.code == code;
        });
}

std::size_t count_diag(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ProjectResourceLoadingDiagnosticCategory category,
    std::string_view code) {
    return static_cast<std::size_t>(std::count_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [category, code](const auto& diagnostic) {
            return diagnostic.category == category && diagnostic.code == code;
        }));
}

} // namespace

TEST_CASE("project resource loader loads trusted skills and prompts", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    write_valid_project_prompt(fix);

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(coding_agent::project_skills_allowed(result.load_plan));
    CHECK(coding_agent::project_prompts_allowed(result.load_plan));
    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].name == "project-skill");
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "review");
    CHECK(result.diagnostics.empty());
}

TEST_CASE("project resource loader skips untrusted resources before parsing adapters", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    fix.write(
        ".cpp-harness/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "this line has no colon\n"
        "description: Bad skill.\n"
        "---\n"
        "Body.\n");
    fix.write(
        ".cpp-harness/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    auto result = fix.load();

    CHECK(result.trust.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    const auto* skill_decision = find_decision(result.load_plan, coding_agent::ProjectResourceKind::ProjectSkills);
    REQUIRE(skill_decision != nullptr);
    CHECK(skill_decision->reason == coding_agent::ResourceSkipReason::Untrusted);
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::SkillAdapter));
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter));
}

TEST_CASE("project resource loader skips disabled project resources without trust or parsing", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    write_valid_project_prompt(fix);

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.policy.project_skills = coding_agent::ResourceEnablement::Off;

    auto result = fix.load(std::move(request));

    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    const auto* skill_decision = find_decision(result.load_plan, coding_agent::ProjectResourceKind::ProjectSkills);
    REQUIRE(skill_decision != nullptr);
    CHECK(skill_decision->reason == coding_agent::ResourceSkipReason::Disabled);
    const auto* prompt_decision = find_decision(result.load_plan, coding_agent::ProjectResourceKind::ProjectPrompts);
    REQUIRE(prompt_decision != nullptr);
    CHECK(prompt_decision->reason == coding_agent::ResourceSkipReason::Disabled);
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::SkillAdapter));
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter));
}

TEST_CASE("project resource loader marks project prompts disabled when prompt templates are off", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    write_valid_project_prompt(fix);

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.prompt_templates_enabled = false;

    auto result = fix.load(std::move(request));

    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    CHECK_FALSE(coding_agent::project_prompts_allowed(result.load_plan));
    const auto* prompt_decision = find_decision(result.load_plan, coding_agent::ProjectResourceKind::ProjectPrompts);
    REQUIRE(prompt_decision != nullptr);
    CHECK(prompt_decision->reason == coding_agent::ResourceSkipReason::Disabled);
    CHECK(result.resources.prompt_templates.empty());
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter));
}

TEST_CASE("project resource loader reports unsupported markers without resolving trust", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    fix.write(".cpp-harness/settings.json", "{}");
    std::filesystem::create_directories(fix.workspace.path() / ".cpp-harness" / "extensions");
    std::filesystem::create_directories(fix.workspace.path() / ".cpp-harness" / "packages");
    fix.write(".cpp-harness/SYSTEM.md", "system");
    fix.write(".cpp-harness/APPEND_SYSTEM.md", "append");
    fix.write("trust.json", "{not json");

    auto result = fix.load();

    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    REQUIRE(result.load_plan.decisions.size() == 5);
    require_decision_reason(
        result.load_plan,
        coding_agent::ProjectResourceKind::ProjectSettings,
        coding_agent::ResourceSkipReason::Unsupported);
    require_decision_reason(
        result.load_plan,
        coding_agent::ProjectResourceKind::ProjectExtensions,
        coding_agent::ResourceSkipReason::Unsupported);
    require_decision_reason(
        result.load_plan,
        coding_agent::ProjectResourceKind::ProjectPackages,
        coding_agent::ResourceSkipReason::Unsupported);
    require_decision_reason(
        result.load_plan,
        coding_agent::ProjectResourceKind::ProjectSystemPrompt,
        coding_agent::ResourceSkipReason::Unsupported);
    require_decision_reason(
        result.load_plan,
        coding_agent::ProjectResourceKind::ProjectAppendSystemPrompt,
        coding_agent::ResourceSkipReason::Unsupported);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    CHECK(result.diagnostics.empty());
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

    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    CHECK(result.load_plan.decisions.empty());
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].name == "explicit");
    CHECK(result.resources.skills.empty());
}

TEST_CASE("project resource loader explicit prompt templates take precedence over project prompts", "[coding_agent][project-resource-loader]") {
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
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::Duplicate, "duplicate_template_skipped"));
}

TEST_CASE("project resource loader surfaces adapter diagnostics for malformed trusted inputs", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    fix.write(
        ".cpp-harness/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "description:\n"
        "---\n"
        "Body.\n");
    fix.write(
        ".cpp-harness/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::SkillAdapter, "invalid_metadata"));
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter, "parse_failed"));
}

TEST_CASE("project resource loader diagnoses project duplicates against host resources", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    write_valid_project_skill(fix, "same-name");
    write_valid_project_prompt(fix, "same-template");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    request.host_skills.push_back(coding_agent::Skill{
        .name = "same-name",
        .description = "Host skill.",
        .content = "Host skill body.",
        .filePath = "/host/same-name/SKILL.md",
    });
    request.host_prompt_templates.push_back(coding_agent::PromptTemplate{
        .name = "same-template",
        .description = "Host template.",
        .content = "Host template body.",
    });

    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.skills.size() == 1);
    CHECK(result.resources.skills[0].content == "Host skill body.");
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(result.resources.prompt_templates[0].content == "Host template body.");
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::Duplicate, "duplicate_skill_skipped"));
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::Duplicate, "duplicate_template_skipped"));
}

TEST_CASE("project resource loader uses stable diagnostic categories and formatted codes", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    write_valid_project_skill(fix);
    fix.write("trust.json", "{not json");

    auto result = fix.load();

    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::Trust, "trust_store_unavailable"));
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::LoadPlan, "project_skills"));

    const auto trust_diag = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diag) {
            return diag.category == coding_agent::ProjectResourceLoadingDiagnosticCategory::Trust;
        });
    REQUIRE(trust_diag != result.diagnostics.end());
    CHECK(coding_agent::project_resource_loading_diagnostic_code(*trust_diag) ==
          "trust:trust_store_unavailable");

    const auto load_plan_diag = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diag) {
            return diag.category == coding_agent::ProjectResourceLoadingDiagnosticCategory::LoadPlan;
        });
    REQUIRE(load_plan_diag != result.diagnostics.end());
    CHECK(coding_agent::project_resource_loading_diagnostic_code(*load_plan_diag) ==
          "resource:project_skills");
}

TEST_CASE("project resource loader classifies adapter duplicate diagnostics once", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    fix.write(
        ".cpp-harness/skills/first/SKILL.md",
        "---\n"
        "name: dupe-skill\n"
        "description: First skill.\n"
        "---\n"
        "First skill body.\n");
    fix.write(
        ".cpp-harness/skills/second/SKILL.md",
        "---\n"
        "name: dupe-skill\n"
        "description: Duplicate skill.\n"
        "---\n"
        "Duplicate skill body.\n");
    fix.write(
        ".cpp-harness/prompts/dupe.md",
        "---\n"
        "description: First prompt.\n"
        "---\n"
        "First prompt body.\n");
    fix.write(
        ".cpp-harness/prompts/dupe.MD",
        "---\n"
        "description: Duplicate prompt.\n"
        "---\n"
        "Duplicate prompt body.\n");

    coding_agent::ProjectResourceLoadingRequest request;
    request.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = fix.load(std::move(request));

    REQUIRE(result.resources.skills.size() == 1);
    REQUIRE(result.resources.prompt_templates.size() == 1);
    CHECK(count_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::Duplicate,
                     "duplicate_skill_skipped") == 1);
    CHECK(count_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::Duplicate,
                     "duplicate_template_skipped") == 1);
    CHECK(count_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::SkillAdapter,
                     "duplicate_name") == 0);
    CHECK(count_diag(result, coding_agent::ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter,
                     "duplicate_name") == 0);
}

TEST_CASE("project resource loader treats explicit prompt template read failure as fatal", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    // No file named missing.md exists.
    coding_agent::ProjectResourceLoadingRequest request;
    request.explicit_prompt_templates.push_back({.path = "missing.md", .is_file = true});

    auto result = fix.load(std::move(request));

    REQUIRE(!result.fatal_errors.empty());
    CHECK(result.fatal_errors[0].category == coding_agent::ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter);
    CHECK(result.resources.prompt_templates.empty());
    CHECK(result.diagnostics.empty());
}
