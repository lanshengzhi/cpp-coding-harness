#pragma once

// Test-only compatibility support for the synchronous loader unit tests. The
// production resource and loader APIs accept only AsyncFileSystem; this file
// keeps the older WorkspaceFileSystem-shaped test fixtures small while they
// migrate to the asynchronous seam.

#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/SkillLoader.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <expected>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests::legacy {

class AsyncFileSystemAdapter final : public harness::AsyncFileSystem {
public:
    explicit AsyncFileSystemAdapter(harness::WorkspaceFileSystem filesystem) : filesystem_(std::move(filesystem)) {}

    [[nodiscard]] const std::filesystem::path& workspace() const override { return filesystem_.root(); }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> absolutePath(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] { return filesystem_.absolutePath(path); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> joinPath(
            std::vector<std::string> parts, std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] { return filesystem_.joinPath(parts); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> readTextFile(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] { return filesystem_.readTextFile(path); });
    }

    [[nodiscard]] support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(
            std::string path, std::optional<int> max_lines, std::stop_token stop_token) override {
        return immediate<std::vector<std::string>>(
                path, stop_token, [&] { return filesystem_.readTextLines(path, max_lines); });
    }

    [[nodiscard]] support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(
            std::string path, std::stop_token stop_token) override {
        return immediate<harness::BinaryData>(path, stop_token, [&] { return filesystem_.readBinaryFile(path); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> writeFile(
            std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.writeFile(path, content); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> appendFile(
            std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.appendFile(path, content); });
    }

    [[nodiscard]] support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(
            std::string path, std::stop_token stop_token) override {
        return immediate<harness::FileInfo>(path, stop_token, [&] { return filesystem_.fileInfo(path); });
    }

    [[nodiscard]] support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::vector<harness::FileInfo>>(path, stop_token, [&] { return filesystem_.listDir(path); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> canonicalPath(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] { return filesystem_.canonicalPath(path); });
    }

    [[nodiscard]] support::AsyncResult<bool, harness::FileError> exists(
            std::string path, std::stop_token stop_token) override {
        return immediate<bool>(path, stop_token, [&] { return filesystem_.exists(path); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> createDir(
            std::string path, bool recursive, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.createDir(path, recursive); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> remove(
            std::string path, bool recursive, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.remove(path, recursive); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> createTempDir(
            std::optional<std::string> prefix, std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] { return filesystem_.createTempDir(prefix); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> createTempFile(
            std::optional<std::string> prefix, std::optional<std::string> suffix, std::stop_token stop_token) override {
        return immediate<std::string>(
                std::nullopt, stop_token, [&] { return filesystem_.createTempFile(prefix, suffix); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> cleanup() override {
        return support::AsyncResult<void, harness::FileError>{std::expected<void, harness::FileError>{}};
    }

private:
    template <typename Value, typename Operation>
    [[nodiscard]] static support::AsyncResult<Value, harness::FileError> immediate(
            std::optional<std::string> path, std::stop_token stop_token, Operation operation) {
        if (stop_token.stop_requested()) {
            return support::AsyncResult<Value, harness::FileError>{std::unexpected(harness::FileError{
                    .code = harness::FileErrorCode::Aborted,
                    .message = "Operation aborted",
                    .path = std::move(path),
            })};
        }
        return support::AsyncResult<Value, harness::FileError>{operation()};
    }

    harness::WorkspaceFileSystem filesystem_;
};

[[nodiscard]] inline std::shared_ptr<harness::AsyncFileSystem> make_async_filesystem(
        const harness::WorkspaceFileSystem& filesystem) {
    return std::make_shared<AsyncFileSystemAdapter>(filesystem);
}

[[nodiscard]] inline std::shared_ptr<harness::AsyncFileSystem> make_async_filesystem(
        const std::filesystem::path& root) {
    return std::make_shared<AsyncFileSystemAdapter>(harness::WorkspaceFileSystem{root});
}

template <typename T, typename E>
[[nodiscard]] inline std::expected<T, E> run_sync(support::AsyncResult<T, E> operation) {
    std::optional<std::expected<T, E>> outcome;
    std::move(operation).start([&outcome](std::expected<T, E> value) noexcept { outcome.emplace(std::move(value)); });
    if (!outcome) {
        std::terminate();
    }
    return std::move(*outcome);
}

[[nodiscard]] inline std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

[[nodiscard]] inline bool path_is_under(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto relative = normalized_absolute(candidate).lexically_relative(normalized_absolute(root));
    if (relative.empty()) {
        return true;
    }
    const auto first = relative.begin();
    return first == relative.end() || *first != "..";
}

[[nodiscard]] inline coding_agent::ProjectResourceFileSystems make_project_resource_filesystems(
        const harness::WorkspaceFileSystem& filesystem, const coding_agent::ProjectResourceLoadingRequest& request) {
    const auto workspace = normalized_absolute(request.workspace.empty() ? filesystem.root() : request.workspace);
    const auto source_root = normalized_absolute(filesystem.root());

    coding_agent::ProjectResourceFileSystems result;
    result.workspace = path_is_under(source_root, workspace) ? make_async_filesystem(workspace)
                                                             : make_async_filesystem(filesystem);

    auto current = workspace;
    std::optional<std::filesystem::path> git_root;
    while (true) {
        if (current != current.root_path()) {
            result.ancestor_roots.push_back(make_async_filesystem(current));
        }
        std::error_code error;
        if (!git_root && std::filesystem::exists(current / ".git", error) && !error) {
            git_root = current;
        }
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    if (git_root && *git_root != git_root->root_path()) {
        result.git_roots.push_back(make_async_filesystem(*git_root));
    }

    if (request.agent_config_directory && !request.agent_config_directory->empty()) {
        result.agent_config_directory = make_async_filesystem(normalized_absolute(*request.agent_config_directory));
    }
    const auto home = request.home_directory.value_or(coding_agent::home_directory());
    result.user_agents_root = make_async_filesystem(normalized_absolute(home / ".agents"));

    std::vector<std::string> explicit_paths = request.skill_paths;
    explicit_paths.insert(explicit_paths.end(), request.theme_paths.begin(), request.theme_paths.end());
    for (const auto& input : request.explicit_prompt_templates) {
        explicit_paths.push_back(input.path);
    }
    if (request.system_prompt && !request.system_prompt->empty()) {
        explicit_paths.push_back(*request.system_prompt);
    }
    for (const auto& input : request.append_system_prompt) {
        if (!input.empty()) {
            explicit_paths.push_back(input);
        }
    }
    for (const auto& raw_path : explicit_paths) {
        const auto raw = std::filesystem::path{raw_path};
        const auto candidate = normalized_absolute(raw.is_absolute() ? raw : workspace / raw);
        std::shared_ptr<harness::AsyncFileSystem> capability;
        if (path_is_under(result.workspace->workspace(), candidate)) {
            capability = result.workspace;
        } else if (result.agent_config_directory &&
                   path_is_under(result.agent_config_directory->workspace(), candidate)) {
            capability = result.agent_config_directory;
        } else if (result.user_agents_root && path_is_under(result.user_agents_root->workspace(), candidate)) {
            capability = result.user_agents_root;
        }
        if (capability) {
            result.explicit_paths.push_back(coding_agent::AuthorizedResourcePath{
                    .path = candidate.string(),
                    .filesystem = std::move(capability),
            });
        }
    }
    return result;
}

} // namespace cch::tests::legacy

// These overloads are deliberately defined in the test support package, not
// in any production header. They let the existing synchronous unit fixtures
// exercise the canonical AsyncFileSystem implementation without restoring a
// production compatibility API.
namespace cch::coding_agent {

[[nodiscard]] inline SkillLoadResult loadSkillFromFile(const harness::WorkspaceFileSystem& filesystem,
        const std::string& file_path,
        SkillSourceContext source_context = {}) {
    auto result = tests::legacy::run_sync(
            loadSkillFromFile(*tests::legacy::make_async_filesystem(filesystem), file_path, std::move(source_context)));
    return result ? std::move(*result) : SkillLoadResult{};
}

[[nodiscard]] inline SkillLoadResult loadSkills(
        const harness::WorkspaceFileSystem& filesystem, const std::vector<SkillDirSpec>& directories) {
    auto result = tests::legacy::run_sync(loadSkills(*tests::legacy::make_async_filesystem(filesystem), directories));
    return result ? std::move(*result) : SkillLoadResult{};
}

[[nodiscard]] inline PromptTemplateLoadResult loadPromptTemplateFromFile(const harness::WorkspaceFileSystem& filesystem,
        const std::string& file_path,
        const std::optional<SourceInfo>& source_info = std::nullopt) {
    auto result = tests::legacy::run_sync(
            loadPromptTemplateFromFile(*tests::legacy::make_async_filesystem(filesystem), file_path, source_info));
    return result ? std::move(*result) : PromptTemplateLoadResult{};
}

[[nodiscard]] inline PromptTemplateLoadResult loadPromptTemplates(
        const harness::WorkspaceFileSystem& filesystem, const std::vector<PromptTemplateDirSpec>& directories) {
    auto result = tests::legacy::run_sync(
            loadPromptTemplates(*tests::legacy::make_async_filesystem(filesystem), directories));
    return result ? std::move(*result) : PromptTemplateLoadResult{};
}

[[nodiscard]] inline ProjectResourceLoadingResult load_project_resources(const harness::WorkspaceFileSystem& filesystem,
        const ProjectTrustStore& trust_store,
        ProjectResourceLoadingRequest request) {
    auto result = tests::legacy::run_sync(load_project_resources(
            tests::legacy::make_project_resource_filesystems(filesystem, request), trust_store, request));
    if (result) {
        return std::move(*result);
    }
    ProjectResourceLoadingResult failure;
    failure.diagnostics.push_back(ResourceDiagnostic{
            .type = ResourceDiagnosticType::Error,
            .message = result.error().message,
            .path = result.error().path,
            .collision = std::nullopt,
    });
    return failure;
}

[[nodiscard]] inline ProjectResourceDetectionResult detect_project_resources(
        const harness::WorkspaceFileSystem& filesystem, const std::filesystem::path& user_agents_skills_dir) {
    ProjectResourceFileSystems capabilities;
    capabilities.workspace = tests::legacy::make_async_filesystem(filesystem);
    auto current = tests::legacy::normalized_absolute(filesystem.root());
    while (true) {
        const auto parent = current.parent_path();
        if (parent == current || parent == parent.root_path()) {
            break;
        }
        current = parent;
        capabilities.ancestor_roots.push_back(tests::legacy::make_async_filesystem(current));
    }
    auto result = tests::legacy::run_sync(detect_project_resources(std::move(capabilities), user_agents_skills_dir));
    if (result) {
        return std::move(*result);
    }
    ProjectResourceDetectionResult failure;
    failure.diagnostics.push_back(ResourceDiagnostic{
            .type = ResourceDiagnosticType::Warning,
            .message = result.error().message,
            .path = result.error().path,
            .collision = std::nullopt,
    });
    return failure;
}

} // namespace cch::coding_agent
