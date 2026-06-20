#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/ProjectResources.hpp"
#include "../../src/harness/WorkspaceFileSystem.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>

using namespace cch;

namespace {

harness::WorkspaceFileSystem fs_for(const tests::TempWorkspace& workspace) {
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs.has_value());
    return *fs;
}

bool detected(const coding_agent::ProjectResourceDetectionResult& result, coding_agent::ProjectResourceKind kind) {
    return coding_agent::has_detected_kind(result, kind);
}

} // namespace

TEST_CASE("project resource detection ignores empty harness and sessions", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "sessions");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(result.resources.empty());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("project resource detection maps protected markers", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    workspace.write(".cpp-harness/settings.json", "{}");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "skills");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "prompts");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "extensions");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "packages");
    workspace.write(".cpp-harness/SYSTEM.md", "system");
    workspace.write(".cpp-harness/APPEND_SYSTEM.md", "append");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSettings));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectPrompts));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectExtensions));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectPackages));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectAppendSystemPrompt));
    CHECK(result.diagnostics.empty());
}

TEST_CASE("project resource detection is case-sensitive", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "Skills");
    workspace.write(".cpp-harness/system.md", "lowercase");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
}

TEST_CASE("project resource load plan gates skills by trust and enablement", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "skills");
    auto detection = coding_agent::detect_project_resources(fs_for(workspace));

    coding_agent::ProjectResourcePolicy policy{};
    CHECK(coding_agent::needs_project_trust_resolution(detection, policy));

    auto trusted_plan = coding_agent::build_project_resource_load_plan(
        detection,
        policy,
        coding_agent::ProjectTrustResolution{
            .decision = coding_agent::ProjectTrustDecision::Trusted,
            .source = coding_agent::ProjectTrustSource::CliOverride,
        });
    CHECK(coding_agent::project_skills_allowed(trusted_plan));

    auto untrusted_plan = coding_agent::build_project_resource_load_plan(
        detection,
        policy,
        coding_agent::ProjectTrustResolution{
            .decision = coding_agent::ProjectTrustDecision::Untrusted,
            .source = coding_agent::ProjectTrustSource::DefaultAskNoUi,
        });
    CHECK_FALSE(coding_agent::project_skills_allowed(untrusted_plan));
    CHECK(untrusted_plan.skipped_for_untrusted);

    policy.project_skills = coding_agent::ResourceEnablement::Off;
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(detection, policy));
    auto disabled_plan = coding_agent::build_project_resource_load_plan(
        detection,
        policy,
        coding_agent::ProjectTrustResolution{
            .decision = coding_agent::ProjectTrustDecision::Trusted,
            .source = coding_agent::ProjectTrustSource::CliOverride,
        });
    CHECK_FALSE(coding_agent::project_skills_allowed(disabled_plan));
    REQUIRE(disabled_plan.decisions.size() == 1);
    CHECK(disabled_plan.decisions[0].reason == coding_agent::ResourceSkipReason::Disabled);
}

TEST_CASE("project resource load plan does not force trust for unsupported future markers", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "extensions");

    auto detection = coding_agent::detect_project_resources(fs_for(workspace));
    coding_agent::ProjectResourcePolicy policy{};

    CHECK_FALSE(coding_agent::needs_project_trust_resolution(detection, policy));
    auto plan = coding_agent::build_project_resource_load_plan(
        detection,
        policy,
        coding_agent::ProjectTrustResolution{
            .decision = coding_agent::ProjectTrustDecision::Trusted,
            .source = coding_agent::ProjectTrustSource::NoProjectResources,
        });
    REQUIRE(plan.decisions.size() == 1);
    CHECK(plan.decisions[0].reason == coding_agent::ResourceSkipReason::Unsupported);
    CHECK_FALSE(plan.decisions[0].allowed);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("project resource detection rejects escaping symlink marker", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    auto outside = std::filesystem::temp_directory_path() / "cch-outside-skills";
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directories(workspace.path() / ".cpp-harness");
    std::filesystem::create_directory_symlink(outside, workspace.path() / ".cpp-harness" / "skills");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics[0].code == "marker_symlink_invalid");
    CHECK_FALSE(result.resources[0].loadable);

    std::filesystem::remove_all(outside);
}
#endif
