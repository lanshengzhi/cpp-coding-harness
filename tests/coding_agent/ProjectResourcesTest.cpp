#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/ProjectResources.hpp"
#include "harness/WorkspaceFileSystem.hpp"
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

TEST_CASE("project resource detection ignores an empty project and sessions dir", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".pi" / "sessions");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(result.resources.empty());
    CHECK(result.diagnostics.empty());
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE(
    "project resource detection maps the .pi/ trust-requiring markers",
    "[coding_agent][project-resources][issue405]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".pi" / "skills");
    std::filesystem::create_directories(workspace.path() / ".pi" / "prompts");
    std::filesystem::create_directories(workspace.path() / ".pi" / "themes");
    workspace.write(".pi/SYSTEM.md", "system");
    workspace.write(".pi/APPEND_SYSTEM.md", "append");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectPrompts));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectThemes));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectAppendSystemPrompt));
    CHECK(result.diagnostics.empty());
    CHECK(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("project resource detection ignores legacy .cpp-harness/ markers with no fallback read", "[coding_agent][project-resources][issue405]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "skills");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "prompts");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "extensions");
    std::filesystem::create_directories(workspace.path() / ".cpp-harness" / "packages");
    workspace.write(".cpp-harness/SYSTEM.md", "system");
    workspace.write(".cpp-harness/APPEND_SYSTEM.md", "append");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(result.resources.empty());
    CHECK(result.diagnostics.empty());
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("project resource detection is case-sensitive", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".pi" / "Skills");
    workspace.write(".pi/system.md", "lowercase");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE(
    "project resource detection treats every loadable marker as trust-requiring",
    "[coding_agent][project-resources][issue405]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/SYSTEM.md", "system");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("project resource detection reports marker kind mismatches and keeps them untrusted", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/skills", "a file where a directory is expected");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics[0].type == coding_agent::ResourceDiagnosticType::Warning);
    CHECK(result.diagnostics[0].message.find("unexpected kind") != std::string::npos);
    CHECK(result.diagnostics[0].path == ".pi/skills");
    CHECK_FALSE(result.resources[0].loadable);
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("project resource detection rejects escaping symlink marker", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    auto outside = std::filesystem::temp_directory_path() / "cch-outside-skills";
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directories(workspace.path() / ".pi");
    std::filesystem::create_directory_symlink(outside, workspace.path() / ".pi" / "skills");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics[0].message.find("marker symlink") != std::string::npos ||
          result.diagnostics[0].message.find("path escapes") != std::string::npos ||
          result.diagnostics[0].message.find("outside") != std::string::npos);
    CHECK_FALSE(result.resources[0].loadable);

    std::filesystem::remove_all(outside);
}
#endif

TEST_CASE("project resource detection maps markers with pi diagnostic shape", "[coding_agent][project-resources][issue405]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/skills", "not a directory");

    auto result = coding_agent::detect_project_resources(fs_for(workspace));

    REQUIRE(result.diagnostics.size() == 1);
    const auto& diagnostic = result.diagnostics[0];
    CHECK(diagnostic.type == coding_agent::ResourceDiagnosticType::Warning);
    CHECK_FALSE(diagnostic.message.empty());
    CHECK(diagnostic.path == ".pi/skills");
    CHECK_FALSE(diagnostic.collision.has_value());
}

TEST_CASE("to_string names the .pi/ marker kinds", "[coding_agent][project-resources]") {
    CHECK(coding_agent::to_string(coding_agent::ProjectResourceKind::ProjectSkills) == "project_skills");
    CHECK(coding_agent::to_string(coding_agent::ProjectResourceKind::ProjectPrompts) == "project_prompts");
    CHECK(coding_agent::to_string(coding_agent::ProjectResourceKind::ProjectThemes) == "project_themes");
    CHECK(coding_agent::to_string(coding_agent::ProjectResourceKind::ProjectSystemPrompt) == "project_system_prompt");
    CHECK(coding_agent::to_string(coding_agent::ProjectResourceKind::ProjectAppendSystemPrompt) ==
          "project_append_system_prompt");
}
