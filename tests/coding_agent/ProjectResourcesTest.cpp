#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/agent/harness/LocalFileSystem.hpp>
#include "agent/harness/RuntimeRoot.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/FakeAsyncFileSystem.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

using namespace cch;

namespace {

harness::WorkspaceFileSystem fs_for(const tests::TempWorkspace& workspace) {
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs.has_value());
    return *fs;
}

/// A user `.agents/skills` directory that cannot collide with any test
/// workspace ancestor (the real `~/.agents/skills` must not gate the
/// hermetic tests).
const std::filesystem::path kTestUserAgentsSkills =
    std::filesystem::path{"/nonexistent-cch-home"} / ".agents" / "skills";

bool detected(const coding_agent::ProjectResourceDetectionResult& result, coding_agent::ProjectResourceKind kind) {
    return coding_agent::has_detected_kind(result, kind);
}

class AsyncDetectionRuntime final {
public:
    AsyncDetectionRuntime()
        : loop_(std::make_shared<boost::asio::io_context>()), root_(loop_, harness::RuntimeLimits{}) {}

    ~AsyncDetectionRuntime() {
        root_.close();
        while (loop_->poll() != 0) {
        }
    }

    template <typename T, typename E> std::expected<T, E> run(support::AsyncResult<T, E> operation) {
        loop_->restart();
        std::optional<std::expected<T, E>> outcome;
        boost::asio::co_spawn(
                *loop_,
                [operation = std::move(operation), &outcome]() mutable -> boost::asio::awaitable<void> {
                    outcome = co_await support::detail::await_async_result(std::move(operation));
                    co_return;
                },
                boost::asio::detached);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!outcome && std::chrono::steady_clock::now() < deadline) {
            if (loop_->stopped()) {
                loop_->restart();
            }
            (void)loop_->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(outcome.has_value());
        return std::move(*outcome);
    }

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() { return root_.make_target(); }

private:
    std::shared_ptr<boost::asio::io_context> loop_;
    harness::RuntimeRoot root_;
};

} // namespace

TEST_CASE("project resource detection ignores an empty project and sessions dir", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".pi" / "sessions");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

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

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

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

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

    CHECK(result.resources.empty());
    CHECK(result.diagnostics.empty());
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("project resource detection is case-sensitive", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".pi" / "Skills");
    workspace.write(".pi/system.md", "lowercase");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE(
    "project resource detection treats every loadable marker as trust-requiring",
    "[coding_agent][project-resources][issue405]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/SYSTEM.md", "system");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("project resource detection reports marker kind mismatches and keeps them untrusted", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/skills", "a file where a directory is expected");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics[0].type == coding_agent::ResourceDiagnosticType::Warning);
    CHECK(result.diagnostics[0].message.find("unexpected kind") != std::string::npos);
    CHECK(result.diagnostics[0].path == ".pi/skills");
    CHECK_FALSE(result.resources[0].loadable);
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("project resource detection rejects escaping symlink marker", "[coding_agent][project-resources]") {
    tests::TempWorkspace workspace;
    auto outside = std::filesystem::temp_directory_path() / "cch-outside-skills";
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directories(workspace.path() / ".pi");
    std::filesystem::create_directory_symlink(outside, workspace.path() / ".pi" / "skills");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectSkills));
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK((result.diagnostics[0].message.find("marker symlink") != std::string::npos ||
           result.diagnostics[0].message.find("path escapes") != std::string::npos ||
           result.diagnostics[0].message.find("outside") != std::string::npos));
    CHECK_FALSE(result.resources[0].loadable);

    std::filesystem::remove_all(outside);
}

TEST_CASE("project resource detection maps markers with pi diagnostic shape", "[coding_agent][project-resources][issue405]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/skills", "not a directory");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

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
    CHECK(coding_agent::to_string(coding_agent::ProjectResourceKind::ProjectAgentsSkills) ==
          "project_agents_skills");
}

TEST_CASE(
    "project resource detection treats a workspace .agents/skills directory as trust-requiring",
    "[coding_agent][project-resources][issue412]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".agents" / "skills");

    auto result = coding_agent::detect_project_resources(fs_for(workspace), kTestUserAgentsSkills);

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectAgentsSkills));
    CHECK(result.diagnostics.empty());
    CHECK(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE(
    "project resource detection walks .agents/skills into workspace ancestors",
    "[coding_agent][project-resources][issue412]") {
    tests::TempWorkspace root;
    std::filesystem::create_directories(root.path() / ".agents" / "skills");
    const auto nested = root.path() / "proj" / "sub";
    std::filesystem::create_directories(nested);
    auto fs = harness::WorkspaceFileSystem::create(nested);
    REQUIRE(fs.has_value());

    auto result = coding_agent::detect_project_resources(*fs, kTestUserAgentsSkills);

    CHECK(detected(result, coding_agent::ProjectResourceKind::ProjectAgentsSkills));
    CHECK(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE(
    "project resource detection excludes the user's own ~/.agents/skills from the walk",
    "[coding_agent][project-resources][issue412]") {
    tests::TempWorkspace root;
    std::filesystem::create_directories(root.path() / ".agents" / "skills");
    const auto nested = root.path() / "proj";
    std::filesystem::create_directories(nested);
    auto fs = harness::WorkspaceFileSystem::create(nested);
    REQUIRE(fs.has_value());

    // The workspace's `.agents/skills` is the user's own directory: the
    // walk must not treat it as a project trust trigger (pi
    // `hasTrustRequiringProjectResources` skips `~/.agents/skills` even when
    // cwd is $HOME).
    const auto user_agents_skills = root.path() / ".agents" / "skills";
    auto result = coding_agent::detect_project_resources(*fs, user_agents_skills);

    CHECK_FALSE(detected(result, coding_agent::ProjectResourceKind::ProjectAgentsSkills));
    CHECK_FALSE(coding_agent::needs_project_trust_resolution(result));
}

TEST_CASE("async project resource detection uses a fake filesystem",
        "[coding_agent][project_resources][async][issue560]") {
    auto filesystem = std::make_shared<tests::FakeAsyncFileSystem>("/workspace");
    filesystem->add_directory(".pi");
    filesystem->add_file(".pi/SYSTEM.md", "system");
    filesystem->add_directory(".pi/skills");

    coding_agent::ProjectResourceFileSystems filesystems;
    filesystems.workspace = filesystem;
    AsyncDetectionRuntime runtime;
    auto result =
            runtime.run(coding_agent::detect_project_resources(std::move(filesystems), "/user/.agents/skills", {}));

    REQUIRE(result);
    CHECK(detected(*result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
    CHECK(detected(*result, coding_agent::ProjectResourceKind::ProjectSkills));
}

TEST_CASE("async project resource detection propagates filesystem failures",
        "[coding_agent][project_resources][async][issue560]") {
    auto filesystem = std::make_shared<tests::FakeAsyncFileSystem>("/workspace");
    filesystem->next_error = harness::FileError{
            .code = harness::FileErrorCode::Busy,
            .message = "filesystem is busy",
            .path = std::string{".pi/skills"},
    };

    coding_agent::ProjectResourceFileSystems filesystems;
    filesystems.workspace = filesystem;
    AsyncDetectionRuntime runtime;
    auto result =
            runtime.run(coding_agent::detect_project_resources(std::move(filesystems), "/user/.agents/skills", {}));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Busy);
    CHECK(result.error().message == "filesystem is busy");
    CHECK(result.error().path == ".pi/skills");
}

TEST_CASE("async project resource detection preserves cancellation as an error",
        "[coding_agent][project_resources][async][issue560]") {
    auto filesystem = std::make_shared<tests::FakeAsyncFileSystem>("/workspace");
    coding_agent::ProjectResourceFileSystems filesystems;
    filesystems.workspace = filesystem;
    std::stop_source stop_source;
    stop_source.request_stop();

    AsyncDetectionRuntime runtime;
    auto result = runtime.run(coding_agent::detect_project_resources(
            std::move(filesystems), "/user/.agents/skills", stop_source.get_token()));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Aborted);
}

TEST_CASE("async project resource detection rejects a missing workspace capability",
        "[coding_agent][project_resources][async][issue560]") {
    AsyncDetectionRuntime runtime;
    auto result = runtime.run(coding_agent::detect_project_resources(
            coding_agent::ProjectResourceFileSystems{}, "/user/.agents/skills", {}));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Invalid);
    CHECK(result.error().message.find("workspace filesystem") != std::string::npos);
}

TEST_CASE("async project resource detection works through the local adapter",
        "[coding_agent][project_resources][async][local][issue560]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/SYSTEM.md", "system");

    AsyncDetectionRuntime runtime;
    coding_agent::ProjectResourceFileSystems filesystems;
    filesystems.workspace = std::make_shared<harness::AsyncLocalFileSystem>(runtime.make_target(), workspace.path());
    auto result = runtime.run(coding_agent::detect_project_resources(
            std::move(filesystems), workspace.path() / ".agents" / "skills", {}));

    REQUIRE(result);
    CHECK(detected(*result, coding_agent::ProjectResourceKind::ProjectSystemPrompt));
}
