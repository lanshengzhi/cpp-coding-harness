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

bool has_diag_source(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ProjectResourceLoadingDiagnosticSource source) {
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [source](const auto& diagnostic) { return diagnostic.source == source; });
}

bool has_diag(
    const coding_agent::ProjectResourceLoadingResult& result,
    coding_agent::ProjectResourceLoadingDiagnosticSource source,
    std::string_view code) {
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [source, code](const auto& diagnostic) {
            return diagnostic.source == source && diagnostic.code == code;
        });
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
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::SkillAdapter));
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::PromptTemplateAdapter));
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
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::SkillAdapter));
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::PromptTemplateAdapter));
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
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::PromptTemplateAdapter));
}

TEST_CASE("project resource loader reports unsupported markers without resolving trust", "[coding_agent][project-resource-loader]") {
    LoaderFixture fix;
    std::filesystem::create_directories(fix.workspace.path() / ".cpp-harness" / "extensions");
    fix.write("trust.json", "{not json");

    auto result = fix.load();

    CHECK(result.trust.source == coding_agent::ProjectTrustSource::NoProjectResources);
    REQUIRE(result.load_plan.decisions.size() == 1);
    CHECK(result.load_plan.decisions[0].kind == coding_agent::ProjectResourceKind::ProjectExtensions);
    CHECK(result.load_plan.decisions[0].reason == coding_agent::ResourceSkipReason::Unsupported);
    CHECK(result.resources.skills.empty());
    CHECK(result.resources.prompt_templates.empty());
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::Trust));
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::SkillAdapter));
    CHECK_FALSE(has_diag_source(result, coding_agent::ProjectResourceLoadingDiagnosticSource::PromptTemplateAdapter));
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
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticSource::SkillAdapter, "invalid_metadata"));
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticSource::PromptTemplateAdapter, "parse_failed"));
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
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticSource::Duplicate, "duplicate_skill_skipped"));
    CHECK(has_diag(result, coding_agent::ProjectResourceLoadingDiagnosticSource::Duplicate, "duplicate_template_skipped"));
}
